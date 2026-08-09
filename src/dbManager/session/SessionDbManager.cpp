#include <dbManager/session/SessionDbManager.h>
#include <utils/BackgroundTaskQueue.h>
#include <algorithm>
#include <sstream>

namespace {
constexpr const char* kLogTag = "[会话持久化]";
}

std::shared_ptr<SessionDbManager> SessionDbManager::getInstance()
{
    static std::shared_ptr<SessionDbManager> instance;
    if (instance == nullptr) {
        instance = std::make_shared<SessionDbManager>();
        try {
            instance->dbClient_ = drogon::app().getDbClient("aichatpg");
            instance->detectDbType();
            instance->enabled_ = (instance->dbClient_ != nullptr);
        } catch (const std::exception& ex) {
            LOG_WARN << kLogTag << " 获取数据库客户端失败，持久化降级为纯内存: " << ex.what();
            instance->enabled_ = false;
        }
    }
    return instance;
}

void SessionDbManager::detectDbType()
{
    std::string dbTypeStr = "postgresql";
    try {
        auto customConfig = drogon::app().getCustomConfig();
        if (customConfig.isMember("dbtype")) {
            dbTypeStr = customConfig["dbtype"].asString();
        }
    } catch (...) {
        // 保持默认
    }

    std::transform(dbTypeStr.begin(), dbTypeStr.end(), dbTypeStr.begin(), ::tolower);

    if (dbTypeStr == "sqlite3" || dbTypeStr == "sqlite") {
        dbType_ = DbType::SQLite3;
    } else if (dbTypeStr == "mysql" || dbTypeStr == "mariadb") {
        dbType_ = DbType::MySQL;
    } else {
        dbType_ = DbType::PostgreSQL;
    }
}

// ========================= 建表 SQL（多库方言）=========================

std::string SessionDbManager::getCreateSessionTableSql() const
{
    switch (dbType_) {
        case DbType::SQLite3:
            return R"(
                CREATE TABLE IF NOT EXISTS chat_session_state (
                    session_id TEXT PRIMARY KEY,
                    api_name TEXT DEFAULT '',
                    api_type INTEGER DEFAULT 0,
                    context_key TEXT DEFAULT '',
                    payload TEXT NOT NULL,
                    created_at INTEGER DEFAULT 0,
                    last_active_at INTEGER DEFAULT 0
                );
            )";
        case DbType::MySQL:
            return R"(
                CREATE TABLE IF NOT EXISTS chat_session_state (
                    session_id VARCHAR(191) PRIMARY KEY,
                    api_name VARCHAR(64) DEFAULT '',
                    api_type INT DEFAULT 0,
                    context_key VARCHAR(191) DEFAULT '',
                    payload LONGTEXT NOT NULL,
                    created_at BIGINT DEFAULT 0,
                    last_active_at BIGINT DEFAULT 0
                ) ENGINE=InnoDB;
            )";
        case DbType::PostgreSQL:
        default:
            return R"(
                CREATE TABLE IF NOT EXISTS chat_session_state (
                    session_id VARCHAR(191) PRIMARY KEY,
                    api_name VARCHAR(64) DEFAULT '',
                    api_type INTEGER DEFAULT 0,
                    context_key VARCHAR(191) DEFAULT '',
                    payload TEXT NOT NULL,
                    created_at BIGINT DEFAULT 0,
                    last_active_at BIGINT DEFAULT 0
                );
            )";
    }
}

std::string SessionDbManager::getCreateSessionIndexSql() const
{
    // 三库通用语法
    return "CREATE INDEX IF NOT EXISTS idx_chat_session_state_last_active "
           "ON chat_session_state (last_active_at);";
}

std::string SessionDbManager::getCreateSessionContextIndexSql() const
{
    // Hash 模式懒加载按 context_key 反查会话；缺失该索引会在表增大后退化为全表扫描。
    // 非唯一索引：context_key 允许为空串（Responses 会话不使用该列）。
    return "CREATE INDEX IF NOT EXISTS idx_chat_session_state_context_key "
           "ON chat_session_state (context_key);";
}

std::string SessionDbManager::getCreateResponseTableSql() const
{
    switch (dbType_) {
        case DbType::SQLite3:
            return R"(
                CREATE TABLE IF NOT EXISTS response_index (
                    response_id TEXT PRIMARY KEY,
                    session_id TEXT NOT NULL,
                    response_body TEXT,
                    has_response INTEGER DEFAULT 0,
                    created_at INTEGER DEFAULT 0
                );
            )";
        case DbType::MySQL:
            return R"(
                CREATE TABLE IF NOT EXISTS response_index (
                    response_id VARCHAR(191) PRIMARY KEY,
                    session_id VARCHAR(191) NOT NULL,
                    response_body LONGTEXT,
                    has_response TINYINT DEFAULT 0,
                    created_at BIGINT DEFAULT 0
                ) ENGINE=InnoDB;
            )";
        case DbType::PostgreSQL:
        default:
            return R"(
                CREATE TABLE IF NOT EXISTS response_index (
                    response_id VARCHAR(191) PRIMARY KEY,
                    session_id VARCHAR(191) NOT NULL,
                    response_body TEXT,
                    has_response SMALLINT DEFAULT 0,
                    created_at BIGINT DEFAULT 0
                );
            )";
    }
}

std::string SessionDbManager::getCreateResponseIndexSql() const
{
    // created_at：保留用于运维排查/历史行盘点。
    return "CREATE INDEX IF NOT EXISTS idx_response_index_created "
           "ON response_index (created_at);";
}

std::string SessionDbManager::getCreateResponseSessionIndexSql() const
{
    // 会话过期时按 session_id 子查询级联删除 response_index，无此索引将退化为全表扫描。
    return "CREATE INDEX IF NOT EXISTS idx_response_index_session "
           "ON response_index (session_id);";
}

bool SessionDbManager::ensureTables(std::string* errorMessage)
{
    if (!dbClient_) {
        enabled_ = false;
        if (errorMessage) *errorMessage = "未获取到数据库客户端";
        return false;
    }

    try {
        dbClient_->execSqlSync(getCreateSessionTableSql());
        dbClient_->execSqlSync(getCreateResponseTableSql());
        // 索引创建失败不致命（MySQL 5.7 不支持 IF NOT EXISTS on index）
        try { dbClient_->execSqlSync(getCreateSessionIndexSql()); } catch (...) {}
        try { dbClient_->execSqlSync(getCreateSessionContextIndexSql()); } catch (...) {}
        try { dbClient_->execSqlSync(getCreateResponseIndexSql()); } catch (...) {}
        try { dbClient_->execSqlSync(getCreateResponseSessionIndexSql()); } catch (...) {}
        enabled_ = true;
        LOG_INFO << kLogTag << " 建表完成: chat_session_state / response_index";
        return true;
    } catch (const std::exception& ex) {
        enabled_ = false;
        if (errorMessage) *errorMessage = std::string("创建会话持久化表失败: ") + ex.what();
        LOG_ERROR << kLogTag << " 建表失败，降级为纯内存: " << ex.what();
        return false;
    }
}

// ========================= JSON 工具 =========================

std::string SessionDbManager::toJsonString(const Json::Value& v)
{
    if (v.isNull()) return "";
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["emitUTF8"] = true;
    return Json::writeString(builder, v);
}

Json::Value SessionDbManager::fromJsonString(const std::string& s)
{
    if (s.empty()) return Json::Value(Json::nullValue);
    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errs;
    std::istringstream iss(s);
    if (!Json::parseFromStream(builder, iss, &root, &errs)) {
        LOG_WARN << kLogTag << " JSON 反序列化失败: " << errs;
        return Json::Value(Json::nullValue);
    }
    return root;
}

// ========================= chat_session_state CRUD =========================

bool SessionDbManager::upsertSession(const SessionRow& row, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || row.sessionId.empty()) return false;

    try {
        const std::string payloadStr = toJsonString(row.payload);
        // 采用「先查后写」的通用路径，避免 PG/MySQL/SQLite 三套 upsert 语法差异带来的兼容风险。
        auto existing = dbClient_->execSqlSync(
            "select session_id from chat_session_state where session_id=$1 limit 1",
            row.sessionId);
        if (existing.empty()) {
            dbClient_->execSqlSync(
                "insert into chat_session_state(session_id, api_name, api_type, context_key, payload, created_at, last_active_at) "
                "values($1, $2, $3, $4, $5, $6, $7)",
                row.sessionId, row.apiName, row.apiType, row.contextKey,
                payloadStr, static_cast<int64_t>(row.createdAt), static_cast<int64_t>(row.lastActiveAt));
        } else {
            dbClient_->execSqlSync(
                "update chat_session_state set api_name=$1, api_type=$2, context_key=$3, payload=$4, last_active_at=$5 "
                "where session_id=$6",
                row.apiName, row.apiType, row.contextKey, payloadStr,
                static_cast<int64_t>(row.lastActiveAt), row.sessionId);
        }
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("写入会话失败: ") + ex.what();
        LOG_WARN << kLogTag << " upsertSession 失败(降级忽略): " << ex.what();
        return false;
    }
}

std::optional<SessionDbManager::SessionRow> SessionDbManager::loadSession(
    const std::string& sessionId, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || sessionId.empty()) return std::nullopt;

    try {
        auto result = dbClient_->execSqlSync(
            "select session_id, api_name, api_type, context_key, payload, created_at, last_active_at "
            "from chat_session_state where session_id=$1 limit 1",
            sessionId);
        if (result.empty()) return std::nullopt;

        SessionRow row;
        const auto& r = result[0];
        row.sessionId    = r["session_id"].as<std::string>();
        row.apiName      = r["api_name"].isNull() ? "" : r["api_name"].as<std::string>();
        row.apiType      = r["api_type"].isNull() ? 0 : r["api_type"].as<int>();
        row.contextKey   = r["context_key"].isNull() ? "" : r["context_key"].as<std::string>();
        row.payload      = fromJsonString(r["payload"].isNull() ? "" : r["payload"].as<std::string>());
        row.createdAt    = r["created_at"].isNull() ? 0 : r["created_at"].as<int64_t>();
        row.lastActiveAt = r["last_active_at"].isNull() ? 0 : r["last_active_at"].as<int64_t>();
        return row;
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("读取会话失败: ") + ex.what();
        LOG_WARN << kLogTag << " loadSession 失败(降级为未命中): " << ex.what();
        return std::nullopt;
    }
}

std::optional<SessionDbManager::SessionRow> SessionDbManager::loadSessionByContextKey(
    const std::string& contextKey, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || contextKey.empty()) return std::nullopt;

    try {
        auto result = dbClient_->execSqlSync(
            "select session_id from chat_session_state where context_key=$1 "
            "order by last_active_at desc limit 1",
            contextKey);
        if (result.empty()) return std::nullopt;
        return loadSession(result[0]["session_id"].as<std::string>(), errorMessage);
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("按上下文键读取会话失败: ") + ex.what();
        LOG_WARN << kLogTag << " loadSessionByContextKey 失败(降级为未命中): " << ex.what();
        return std::nullopt;
    }
}

bool SessionDbManager::deleteSession(const std::string& sessionId, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || sessionId.empty()) return false;
    try {
        dbClient_->execSqlSync("delete from chat_session_state where session_id=$1", sessionId);
        // 级联清理该会话下的 responseId 映射，避免悬挂索引指向已删会话。
        dbClient_->execSqlSync("delete from response_index where session_id=$1", sessionId);
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("删除会话失败: ") + ex.what();
        LOG_WARN << kLogTag << " deleteSession 失败(降级忽略): " << ex.what();
        return false;
    }
}

bool SessionDbManager::deleteSessions(const std::vector<std::string>& sessionIds, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || sessionIds.empty()) return false;
    bool allOk = true;
    for (const auto& id : sessionIds) {
        if (!deleteSession(id, errorMessage)) allOk = false;
    }
    return allOk;
}

int SessionDbManager::deleteSessionsOlderThan(int64_t cutoffEpochSeconds, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_) return -1;
    try {
        // 先级联清理响应索引，再删会话，保证不留悬挂映射。
        dbClient_->execSqlSync(
            "delete from response_index where session_id in "
            "(select session_id from chat_session_state where last_active_at > 0 and last_active_at < $1)",
            cutoffEpochSeconds);
        auto result = dbClient_->execSqlSync(
            "delete from chat_session_state where last_active_at > 0 and last_active_at < $1",
            cutoffEpochSeconds);
        return static_cast<int>(result.affectedRows());
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("按时间清理会话失败: ") + ex.what();
        LOG_WARN << kLogTag << " deleteSessionsOlderThan 失败(降级忽略): " << ex.what();
        return -1;
    }
}

// ========================= response_index CRUD =========================

bool SessionDbManager::upsertResponse(const ResponseRow& row, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || row.responseId.empty()) return false;

    try {
        const std::string bodyStr = row.hasResponse ? toJsonString(row.response) : std::string();
        const int hasResp = row.hasResponse ? 1 : 0;

        auto existing = dbClient_->execSqlSync(
            "select response_id, has_response from response_index where response_id=$1 limit 1",
            row.responseId);
        if (existing.empty()) {
            dbClient_->execSqlSync(
                "insert into response_index(response_id, session_id, response_body, has_response, created_at) "
                "values($1, $2, $3, $4, $5)",
                row.responseId, row.sessionId, bodyStr, hasResp,
                static_cast<int64_t>(row.createdAt));
        } else if (row.hasResponse) {
            dbClient_->execSqlSync(
                "update response_index set session_id=$1, response_body=$2, has_response=1 where response_id=$3",
                row.sessionId, bodyStr, row.responseId);
        } else {
            // 仅重绑 sessionId（如会话转移），不得覆盖已存在的响应体。
            dbClient_->execSqlSync(
                "update response_index set session_id=$1 where response_id=$2",
                row.sessionId, row.responseId);
        }
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("写入响应索引失败: ") + ex.what();
        LOG_WARN << kLogTag << " upsertResponse 失败(降级忽略): " << ex.what();
        return false;
    }
}

std::optional<SessionDbManager::ResponseRow> SessionDbManager::loadResponse(
    const std::string& responseId, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || responseId.empty()) return std::nullopt;

    try {
        auto result = dbClient_->execSqlSync(
            "select response_id, session_id, response_body, has_response, created_at "
            "from response_index where response_id=$1 limit 1",
            responseId);
        if (result.empty()) return std::nullopt;

        ResponseRow row;
        const auto& r = result[0];
        row.responseId  = r["response_id"].as<std::string>();
        row.sessionId   = r["session_id"].isNull() ? "" : r["session_id"].as<std::string>();
        row.hasResponse = !r["has_response"].isNull() && r["has_response"].as<int>() != 0;
        if (row.hasResponse) {
            row.response = fromJsonString(r["response_body"].isNull() ? "" : r["response_body"].as<std::string>());
            if (row.response.isNull()) row.hasResponse = false;
        }
        row.createdAt = r["created_at"].isNull() ? 0 : r["created_at"].as<int64_t>();
        return row;
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("读取响应索引失败: ") + ex.what();
        LOG_WARN << kLogTag << " loadResponse 失败(降级为未命中): " << ex.what();
        return std::nullopt;
    }
}

bool SessionDbManager::deleteResponse(const std::string& responseId, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || responseId.empty()) return false;
    try {
        dbClient_->execSqlSync("delete from response_index where response_id=$1", responseId);
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("删除响应索引失败: ") + ex.what();
        LOG_WARN << kLogTag << " deleteResponse 失败(降级忽略): " << ex.what();
        return false;
    }
}

bool SessionDbManager::deleteResponses(const std::vector<std::string>& responseIds, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || responseIds.empty()) return false;
    bool allOk = true;
    for (const auto& id : responseIds) {
        if (!deleteResponse(id, errorMessage)) allOk = false;
    }
    return allOk;
}

int SessionDbManager::deleteResponsesOlderThan(int64_t cutoffEpochSeconds, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_) return -1;
    try {
        auto result = dbClient_->execSqlSync(
            "delete from response_index where created_at > 0 and created_at < $1",
            cutoffEpochSeconds);
        return static_cast<int>(result.affectedRows());
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("按时间清理响应索引失败: ") + ex.what();
        LOG_WARN << kLogTag << " deleteResponsesOlderThan 失败(降级忽略): " << ex.what();
        return -1;
    }
}

// ========================= 异步写穿（BackgroundTaskQueue）=========================

void SessionDbManager::asyncUpsertSession(const SessionRow& row)
{
    if (!enabled_ || row.sessionId.empty()) return;
    auto self = getInstance();
    BackgroundTaskQueue::instance().enqueue("session.upsert", [self, row]() {
        self->upsertSession(row);
    });
}

void SessionDbManager::asyncUpsertResponse(const ResponseRow& row)
{
    if (!enabled_ || row.responseId.empty()) return;
    auto self = getInstance();
    BackgroundTaskQueue::instance().enqueue("responseIndex.upsert", [self, row]() {
        self->upsertResponse(row);
    });
}

void SessionDbManager::asyncDeleteSession(const std::string& sessionId)
{
    if (!enabled_ || sessionId.empty()) return;
    auto self = getInstance();
    BackgroundTaskQueue::instance().enqueue("session.delete", [self, sessionId]() {
        self->deleteSession(sessionId);
    });
}

void SessionDbManager::asyncDeleteSessions(const std::vector<std::string>& sessionIds)
{
    if (!enabled_ || sessionIds.empty()) return;
    auto self = getInstance();
    BackgroundTaskQueue::instance().enqueue("session.deleteBatch", [self, sessionIds]() {
        self->deleteSessions(sessionIds);
    });
}

void SessionDbManager::asyncDeleteResponses(const std::vector<std::string>& responseIds)
{
    if (!enabled_ || responseIds.empty()) return;
    auto self = getInstance();
    BackgroundTaskQueue::instance().enqueue("responseIndex.deleteBatch", [self, responseIds]() {
        self->deleteResponses(responseIds);
    });
}
