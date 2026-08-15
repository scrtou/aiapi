#ifndef CHAYNS_THREAD_DB_MANAGER_H
#define CHAYNS_THREAD_DB_MANAGER_H

#include <infrastructure/persistence/chaynsThread/IChaynsThreadLedger.h>

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Row.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
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
class IBackgroundExecutor;

class chaynsThreadDbManager : public chayns::IChaynsThreadLedger,
                              public std::enable_shared_from_this<chaynsThreadDbManager>
{
public:
    enum class DbType { PostgreSQL, MySQL, SQLite3 };

    using ThreadRow = chayns::ThreadLedgerRow;

    /// Context-owned thread ledger.  The executor is borrowed from the same
    /// AppContext and remains alive until all queued write-through work is
    /// drained during shutdown.
    explicit chaynsThreadDbManager(IBackgroundExecutor* executor = nullptr)
        : executor_(executor) {}
    ~chaynsThreadDbManager() = default;

    /// Acquire the configured drogon DB client after composition-root setup.
    /// Keeping construction side-effect free makes a disabled ledger cheap in
    /// tests and prevents accidental process-global initialization.
    void initialize();

    /// 建表 + 建索引；失败返回 false 并通过 errorMessage 输出原因
    bool ensureTable(std::string* errorMessage = nullptr);

    bool isEnabled() const override { return enabled_; }
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
                                                   std::string* errorMessage = nullptr) override;

    bool deleteThread(const std::string& threadId, std::string* errorMessage = nullptr);
    bool deleteThreads(const std::vector<std::string>& threadIds, std::string* errorMessage = nullptr);

    /// 累加删除重试计数，返回累加后的值；失败返回 -1
    int bumpDeleteAttempts(const std::string& threadId, std::string* errorMessage = nullptr);
    /// 清理重试耗尽的行，返回删除行数；失败返回 -1
    int purgeExhaustedThreads(int maxAttempts, std::string* errorMessage = nullptr);

    // ---- 异步写穿接口（供请求链路使用，永不阻塞）----
    void asyncUpsertThread(const ThreadRow& row) override;
    void asyncDetachThreadBySessionId(const std::string& sessionId) override;
    void asyncUpdateThreadSessionId(const std::string& oldSessionId,
                                    const std::string& newSessionId) override;
    void asyncTouchThreadBySessionId(const std::string& sessionId, int64_t nowEpochSeconds);
    void asyncDeleteThread(const std::string& threadId);

private:
    struct PendingWrite
    {
        std::string name;
        std::function<void()> task;
    };

    void submitWrite(const char* taskName, std::function<void()> task);
    void drainWrites();
    void detectDbType();
    static ThreadRow rowFromRecord(const drogon::orm::Row& r);

    std::string getCreateThreadTableSql() const;
    std::string getCreateThreadActiveIndexSql() const;
    std::string getCreateThreadSessionIndexSql() const;
    bool ensureThreadContextColumns(std::string* errorMessage);

    drogon::orm::DbClientPtr dbClient_;
    IBackgroundExecutor* executor_ = nullptr;  // borrowed; owned by AppContext
    DbType dbType_ = DbType::PostgreSQL;
    std::atomic<bool> enabled_{false};
    bool initialized_ = false;

    // The shared executor has multiple workers.  A single bounded local FIFO
    // preserves upsert(old key) -> rotate(new key) order without parking its
    // other workers behind a condition variable.
    static constexpr std::size_t kMaxPendingWrites = 1024;
    std::mutex writeQueueMutex_;
    std::deque<PendingWrite> pendingWrites_;
    bool writeDrainScheduled_ = false;
};

#endif  // CHAYNS_THREAD_DB_MANAGER_H
