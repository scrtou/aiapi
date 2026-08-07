#ifndef SESSION_DBMANAGER_H
#define SESSION_DBMANAGER_H

#include <drogon/drogon.h>
#include <json/json.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <dbManager/DbType.h>

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
 * - 所有写入由 BackgroundTaskQueue 异步执行，避免污染响应时延。
 * - 多库适配：PostgreSQL / SQLite3 / MySQL 三种方言分别提供建表与 upsert 语句。
 */
class SessionDbManager {
public:
    struct SessionRow {
        std::string sessionId;
        std::string apiName;
        int apiType = 0;          // 0=ChatCompletions, 1=Responses
        std::string contextKey;   // contextConversationId（Hash 模式）
        Json::Value payload;      // session_st 快照
        int64_t createdAt = 0;
        int64_t lastActiveAt = 0;
    };

    struct ResponseRow {
        std::string responseId;
        std::string sessionId;
        Json::Value response;     // 可为 null 表示未存响应体
        bool hasResponse = false;
        int64_t createdAt = 0;
    };

    static std::shared_ptr<SessionDbManager> getInstance();

    /// 建表（幂等）。失败时返回 false 并置 errorMessage，不抛异常。
    bool ensureTables(std::string* errorMessage = nullptr);

    /// 持久化开关：DB 客户端不可用或建表失败时为 false，此时所有读写静默跳过。
    bool isEnabled() const { return enabled_; }
    void setEnabled(bool enabled) { enabled_ = enabled; }

    // ========== chat_session_state ==========
    bool upsertSession(const SessionRow& row, std::string* errorMessage = nullptr);
    std::optional<SessionRow> loadSession(const std::string& sessionId, std::string* errorMessage = nullptr);
    /// 通过 Hash 模式的 context_key 反查会话（context_map 的持久化替身）
    std::optional<SessionRow> loadSessionByContextKey(const std::string& contextKey, std::string* errorMessage = nullptr);
    bool deleteSession(const std::string& sessionId, std::string* errorMessage = nullptr);
    bool deleteSessions(const std::vector<std::string>& sessionIds, std::string* errorMessage = nullptr);
    /// 按 lastActiveAt 早于 cutoff 清理，返回删除行数（-1 表示失败）
    int deleteSessionsOlderThan(int64_t cutoffEpochSeconds, std::string* errorMessage = nullptr);

    // ========== response_index ==========
    bool upsertResponse(const ResponseRow& row, std::string* errorMessage = nullptr);
    std::optional<ResponseRow> loadResponse(const std::string& responseId, std::string* errorMessage = nullptr);
    bool deleteResponse(const std::string& responseId, std::string* errorMessage = nullptr);
    bool deleteResponses(const std::vector<std::string>& responseIds, std::string* errorMessage = nullptr);
    int deleteResponsesOlderThan(int64_t cutoffEpochSeconds, std::string* errorMessage = nullptr);

    // ========== 异步写穿入口（经 BackgroundTaskQueue）==========
    void asyncUpsertSession(const SessionRow& row);
    void asyncUpsertResponse(const ResponseRow& row);
    void asyncDeleteSession(const std::string& sessionId);
    void asyncDeleteSessions(const std::vector<std::string>& sessionIds);
    void asyncDeleteResponses(const std::vector<std::string>& responseIds);

    DbType getDbType() const { return dbType_; }

    // 供测试注入：允许在无 drogon app 环境下禁用
    void resetForTest() { enabled_ = false; dbClient_.reset(); }

private:
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
    DbType dbType_ = DbType::PostgreSQL;
    bool enabled_ = false;
};

#endif // 头文件保护结束
