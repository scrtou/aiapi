#ifndef CHAYNS_THREAD_DB_MANAGER_H
#define CHAYNS_THREAD_DB_MANAGER_H

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Row.h>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief chayns 上游线程台账
 *
 * 背景：chaynsapi 的 m_threadMap 只活在进程内存里，但它映射的 thread
 * 是上游 cube.tobit.cloud 上的真实资源。进程重启、会话过期、请求失败
 * 三条路径都会让内存映射消失，而上游 thread 永远留在账号里 —— 这就是泄漏。
 *
 * 本表把 thread_id 单独留痕，与会话生命周期解耦：
 *   - 请求路径只做异步写穿（upsert / detach / touch），绝不阻塞在 DB 或 HTTP 上；
 *   - 真正的上游删除由 ThreadReaper 后台按 last_active_at 批量回收；
 *   - DB 不可用时整体降级为纯内存，chayns 聊天功能不受影响。
 */
class chaynsThreadDbManager
{
public:
    enum class DbType { PostgreSQL, MySQL, SQLite3 };

    /// 线程台账行
    struct ThreadRow
    {
        std::string threadId;         ///< 上游 thread id（主键）
        std::string sessionId;        ///< 绑定的会话键；空串表示已解绑，等待回收
        std::string accountUserName;  ///< 创建线程的账号，删除时必须用同一账号
        std::string origin;           ///< free/pro 路由不同，删除请求需还原原始 Origin
        std::string referer;
        int64_t     createdAt = 0;
        int64_t     lastActiveAt = 0;
        int         deleteAttempts = 0;  ///< 上游删除失败次数，超阈值按孤儿丢弃
    };

    static std::shared_ptr<chaynsThreadDbManager> getInstance();

    chaynsThreadDbManager() = default;
    ~chaynsThreadDbManager() = default;

    /// 建表 + 建索引；失败返回 false 并通过 errorMessage 输出原因
    bool ensureTable(std::string* errorMessage = nullptr);

    bool isEnabled() const { return enabled_; }
    void setEnabled(bool enabled) { enabled_ = enabled; }
    DbType getDbType() const { return dbType_; }

    // ---- 同步接口（供 reaper / 运维路径使用）----
    bool upsertThread(const ThreadRow& row, std::string* errorMessage = nullptr);
    bool detachThreadBySessionId(const std::string& sessionId, std::string* errorMessage = nullptr);
    bool updateThreadSessionId(const std::string& oldSessionId,
                               const std::string& newSessionId,
                               std::string* errorMessage = nullptr);
    bool touchThreadBySessionId(const std::string& sessionId,
                                int64_t nowEpochSeconds,
                                std::string* errorMessage = nullptr);

    std::vector<ThreadRow> loadThreadsOlderThan(int64_t cutoffEpochSeconds,
                                                int limit,
                                                std::string* errorMessage = nullptr);
    std::optional<ThreadRow> loadThreadBySessionId(const std::string& sessionId,
                                                   std::string* errorMessage = nullptr);

    bool deleteThread(const std::string& threadId, std::string* errorMessage = nullptr);
    bool deleteThreads(const std::vector<std::string>& threadIds, std::string* errorMessage = nullptr);

    /// 累加删除重试计数，返回累加后的值；失败返回 -1
    int bumpDeleteAttempts(const std::string& threadId, std::string* errorMessage = nullptr);
    /// 清理重试耗尽的行，返回删除行数；失败返回 -1
    int purgeExhaustedThreads(int maxAttempts, std::string* errorMessage = nullptr);

    // ---- 异步写穿接口（供请求链路使用，永不阻塞）----
    void asyncUpsertThread(const ThreadRow& row);
    void asyncDetachThreadBySessionId(const std::string& sessionId);
    void asyncUpdateThreadSessionId(const std::string& oldSessionId, const std::string& newSessionId);
    void asyncTouchThreadBySessionId(const std::string& sessionId, int64_t nowEpochSeconds);
    void asyncDeleteThread(const std::string& threadId);

private:
    void detectDbType();
    static ThreadRow rowFromRecord(const drogon::orm::Row& r);

    std::string getCreateThreadTableSql() const;
    std::string getCreateThreadActiveIndexSql() const;
    std::string getCreateThreadSessionIndexSql() const;

    drogon::orm::DbClientPtr dbClient_;
    DbType dbType_ = DbType::PostgreSQL;
    std::atomic<bool> enabled_{false};
};

#endif  // CHAYNS_THREAD_DB_MANAGER_H
