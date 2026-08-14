#ifndef SESSION_DBMANAGER_H
#define SESSION_DBMANAGER_H

#include <drogon/drogon.h>
#include <functional>
#include <json/json.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <dbManager/DbType.h>
#include <domain/port/ISessionPersistence.h>

/**
 * @brief 会话持久化管理器（chat_session_state / response_index）
 *
 * 职责：
 * - chat_session_state: 持久化会话快照（session_st 的 JSON 序列化），支撑重启后续聊。
 * - response_index:     持久化 responseId -> sessionId 映射及可选响应体，支撑
 *                       previous_response_id 在重启后仍可续接。
 *
 * 设计约束：
 * - 内存仍是唯一热路径来源；数据库只做「写穿 + 懒加载回填」，任何 DB 故障都必须降级为
 *   「按新会话处理」，绝不阻塞或抛出到请求链路。
 * - 所有写入经构造注入的 IBackgroundExecutor 异步执行，避免污染响应时延。
 * - 多库适配：PostgreSQL / SQLite3 / MySQL 三种方言分别提供建表与 upsert 语句。
 */
class IBackgroundExecutor;

class SessionDbManager : public ISessionPersistence,
                         public std::enable_shared_from_this<SessionDbManager> {
public:
    using SessionRow = SessionPersistenceRow;
    using ResponseRow = ResponsePersistenceRow;

    /// The composition root constructs this store and supplies the executor
    /// before it is published to ResponseIndex/chatSession.
    explicit SessionDbManager(IBackgroundExecutor* executor = nullptr);

    /// Bind the drogon DB client once configuration is loaded.  Construction
    /// itself is side-effect free so fakes/local fixtures do not accidentally
    /// reach into the process-level drogon app.
    void initialize();

    /// 建表（幂等）。失败时返回 false 并置 errorMessage，不抛异常。
    bool ensureTables(std::string* errorMessage = nullptr) override;

    /// 持久化开关：DB 客户端不可用或建表失败时为 false，此时所有读写静默跳过。
    bool isEnabled() const override { return enabled_; }
    void setEnabled(bool enabled) override { enabled_ = enabled; }

    // ========== chat_session_state ==========
    bool upsertSession(const SessionRow& row, std::string* errorMessage = nullptr);
    std::optional<SessionRow> loadSession(const std::string& sessionId, std::string* errorMessage = nullptr) override;
    /// 通过 Hash 模式的 context_key 反查会话（context_map 的持久化替身）
    std::optional<SessionRow> loadSessionByContextKey(const std::string& contextKey, std::string* errorMessage = nullptr) override;
    bool deleteSession(const std::string& sessionId, std::string* errorMessage = nullptr);
    bool deleteSessions(const std::vector<std::string>& sessionIds, std::string* errorMessage = nullptr);
    /// 按 lastActiveAt 早于 cutoff 清理，返回删除行数（-1 表示失败）
    int deleteSessionsOlderThan(int64_t cutoffEpochSeconds, std::string* errorMessage = nullptr) override;

    // ========== response_index ==========
    bool upsertResponse(const ResponseRow& row, std::string* errorMessage = nullptr);
    std::optional<ResponseRow> loadResponse(const std::string& responseId, std::string* errorMessage = nullptr) override;
    bool deleteResponse(const std::string& responseId, std::string* errorMessage = nullptr);
    bool deleteResponses(const std::vector<std::string>& responseIds, std::string* errorMessage = nullptr);
    int deleteResponsesOlderThan(int64_t cutoffEpochSeconds, std::string* errorMessage = nullptr);

    // ========== 异步写穿入口（经注入 executor）==========
    void asyncUpsertSession(const SessionRow& row) override;
    void asyncUpsertResponse(const ResponseRow& row) override;
    void asyncDeleteSession(const std::string& sessionId);
    void asyncDeleteSessions(const std::vector<std::string>& sessionIds) override;
    void asyncDeleteResponses(const std::vector<std::string>& responseIds) override;

    DbType getDbType() const { return dbType_; }

    // 供测试注入：允许在无 drogon app 环境下禁用
    void resetForTest() { enabled_ = false; initialized_ = false; dbClient_.reset(); }

private:
    void submitWrite(const char* taskName, std::function<void()> task);
    void detectDbType();
    std::string getCreateSessionTableSql() const;
    std::string getCreateSessionIndexSql() const;
    std::string getCreateSessionContextIndexSql() const;
    std::string getCreateResponseTableSql() const;
    std::string getCreateResponseIndexSql() const;
    std::string getCreateResponseSessionIndexSql() const;

    static std::string toJsonString(const Json::Value& v);
    static Json::Value fromJsonString(const std::string& s);

    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    IBackgroundExecutor* executor_ = nullptr;  // borrowed; AppContext outlives this store
    DbType dbType_ = DbType::PostgreSQL;
    bool enabled_ = false;
    bool initialized_ = false;
};

#endif // 头文件保护结束
