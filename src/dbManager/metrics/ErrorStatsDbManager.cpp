#include "ErrorStatsDbManager.h"
#include <algorithm>
#include <sstream>

namespace metrics {

// PostgreSQL 建表语句 - 必须拆分执行（PG 不允许多条命令）
static const char* CREATE_ERROR_EVENT_PG_TABLE = R"(
CREATE TABLE IF NOT EXISTS error_event (
    id BIGSERIAL PRIMARY KEY,
    ts TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    severity VARCHAR(10) NOT NULL,
    domain VARCHAR(32) NOT NULL,
    type VARCHAR(64) NOT NULL,
    provider VARCHAR(64),
    model VARCHAR(128),
    client_type VARCHAR(64),
    api_kind VARCHAR(32),
    stream BOOLEAN,
    http_status INTEGER,
    request_id VARCHAR(128),
    response_id VARCHAR(256),
    tool_name VARCHAR(128),
    message TEXT,
    detail_json JSONB,
    raw_snippet TEXT
);
)";

static const char* CREATE_ERROR_EVENT_PG_IDX1 =
    "CREATE INDEX IF NOT EXISTS idx_error_event_ts ON error_event(ts DESC)";
static const char* CREATE_ERROR_EVENT_PG_IDX2 =
    "CREATE INDEX IF NOT EXISTS idx_error_event_filter ON error_event(domain, type, severity, provider, model, client_type, ts DESC)";
static const char* CREATE_ERROR_EVENT_PG_IDX3 =
    "CREATE INDEX IF NOT EXISTS idx_error_event_request ON error_event(request_id)";

static const char* CREATE_ERROR_AGG_HOUR_PG = R"(
CREATE TABLE IF NOT EXISTS error_agg_hour (
    bucket_start TIMESTAMPTZ NOT NULL,
    severity VARCHAR(10) NOT NULL,
    domain VARCHAR(32) NOT NULL,
    type VARCHAR(64) NOT NULL,
    provider VARCHAR(64) NOT NULL DEFAULT '',
    model VARCHAR(128) NOT NULL DEFAULT '',
    client_type VARCHAR(64) NOT NULL DEFAULT '',
    api_kind VARCHAR(32) NOT NULL DEFAULT '',
    stream BOOLEAN NOT NULL DEFAULT false,
    http_status INTEGER NOT NULL DEFAULT 0,
    count BIGINT NOT NULL DEFAULT 0,
    last_event_ts TIMESTAMPTZ,
    PRIMARY KEY (bucket_start, severity, domain, type, provider, model, client_type, api_kind, stream, http_status)
);
)";

static const char* CREATE_REQUEST_AGG_HOUR_PG = R"(
CREATE TABLE IF NOT EXISTS request_agg_hour (
    bucket_start TIMESTAMPTZ NOT NULL,
    provider VARCHAR(64) NOT NULL DEFAULT '',
    model VARCHAR(128) NOT NULL DEFAULT '',
    client_type VARCHAR(64) NOT NULL DEFAULT '',
    api_kind VARCHAR(32) NOT NULL DEFAULT '',
    stream BOOLEAN NOT NULL DEFAULT false,
    http_status INTEGER NOT NULL DEFAULT 0,
    count BIGINT NOT NULL DEFAULT 0,
    last_request_ts TIMESTAMPTZ,
    PRIMARY KEY (bucket_start, provider, model, client_type, api_kind, stream, http_status)
);
)";

// SQLite3 建表语句 - 拆分成单独的语句
static const char* CREATE_ERROR_EVENT_SQLITE_TABLE = R"(
CREATE TABLE IF NOT EXISTS error_event (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    severity TEXT NOT NULL,
    domain TEXT NOT NULL,
    type TEXT NOT NULL,
    provider TEXT,
    model TEXT,
    client_type TEXT,
    api_kind TEXT,
    stream INTEGER,
    http_status INTEGER,
    request_id TEXT,
    response_id TEXT,
    tool_name TEXT,
    message TEXT,
    detail_json TEXT,
    raw_snippet TEXT
)
)";

static const char* CREATE_ERROR_EVENT_SQLITE_IDX1 =
    "CREATE INDEX IF NOT EXISTS idx_error_event_ts ON error_event(ts DESC)";
static const char* CREATE_ERROR_EVENT_SQLITE_IDX2 =
    "CREATE INDEX IF NOT EXISTS idx_error_event_filter ON error_event(domain, type, severity, provider, model, client_type, ts DESC)";
static const char* CREATE_ERROR_EVENT_SQLITE_IDX3 =
    "CREATE INDEX IF NOT EXISTS idx_error_event_request ON error_event(request_id)";

static const char* CREATE_ERROR_AGG_HOUR_SQLITE = R"(
CREATE TABLE IF NOT EXISTS error_agg_hour (
    bucket_start DATETIME NOT NULL,
    severity TEXT NOT NULL,
    domain TEXT NOT NULL,
    type TEXT NOT NULL,
    provider TEXT NOT NULL DEFAULT '',
    model TEXT NOT NULL DEFAULT '',
    client_type TEXT NOT NULL DEFAULT '',
    api_kind TEXT NOT NULL DEFAULT '',
    stream INTEGER NOT NULL DEFAULT 0,
    http_status INTEGER NOT NULL DEFAULT 0,
    count INTEGER NOT NULL DEFAULT 0,
    last_event_ts DATETIME,
    PRIMARY KEY (bucket_start, severity, domain, type, provider, model, client_type, api_kind, stream, http_status)
)
)";

static const char* CREATE_REQUEST_AGG_HOUR_SQLITE = R"(
CREATE TABLE IF NOT EXISTS request_agg_hour (
    bucket_start DATETIME NOT NULL,
    provider TEXT NOT NULL DEFAULT '',
    model TEXT NOT NULL DEFAULT '',
    client_type TEXT NOT NULL DEFAULT '',
    api_kind TEXT NOT NULL DEFAULT '',
    stream INTEGER NOT NULL DEFAULT 0,
    http_status INTEGER NOT NULL DEFAULT 0,
    count INTEGER NOT NULL DEFAULT 0,
    last_request_ts DATETIME,
    PRIMARY KEY (bucket_start, provider, model, client_type, api_kind, stream, http_status)
)
)";

std::shared_ptr<ErrorStatsDbManager> ErrorStatsDbManager::getInstance() {
    static std::shared_ptr<ErrorStatsDbManager> instance = []() {
        auto inst = std::make_shared<ErrorStatsDbManager>();
        inst->init();
        return inst;
    }();
    return instance;
}

void ErrorStatsDbManager::detectDbType() {
    auto customConfig = drogon::app().getCustomConfig();
    std::string dbTypeStr = "postgresql";
    if (customConfig.isMember("dbtype")) {
        dbTypeStr = customConfig["dbtype"].asString();
    }
    std::transform(dbTypeStr.begin(), dbTypeStr.end(), dbTypeStr.begin(), ::tolower);
    if (dbTypeStr == "sqlite3" || dbTypeStr == "sqlite") {
        dbType_ = DbType::SQLite3;
        LOG_INFO << "[错误统计数据库] 数据库类型：SQLite3";
    } else {
        dbType_ = DbType::PostgreSQL;
        LOG_INFO << "[错误统计数据库] 数据库类型：PostgreSQL";
    }
}

void ErrorStatsDbManager::init() {
    LOG_INFO << "[错误统计数据库] 初始化开始";
    dbClient_ = drogon::app().getDbClient("aichatpg");
    detectDbType();
    createTablesIfNotExist();
    LOG_INFO << "[错误统计数据库] 初始化完成";
}

void ErrorStatsDbManager::createTablesIfNotExist() {
    try {
        if (dbType_ == DbType::SQLite3) {
            // SQLite3 需要分开执行每条语句
            dbClient_->execSqlSync(CREATE_ERROR_EVENT_SQLITE_TABLE);
            dbClient_->execSqlSync(CREATE_ERROR_EVENT_SQLITE_IDX1);
            dbClient_->execSqlSync(CREATE_ERROR_EVENT_SQLITE_IDX2);
            dbClient_->execSqlSync(CREATE_ERROR_EVENT_SQLITE_IDX3);
            dbClient_->execSqlSync(CREATE_ERROR_AGG_HOUR_SQLITE);
            dbClient_->execSqlSync(CREATE_REQUEST_AGG_HOUR_SQLITE);
        } else {
            // PostgreSQL 同样需要逐条执行，避免多语句执行限制
            dbClient_->execSqlSync(CREATE_ERROR_EVENT_PG_TABLE);
            dbClient_->execSqlSync(CREATE_ERROR_EVENT_PG_IDX1);
            dbClient_->execSqlSync(CREATE_ERROR_EVENT_PG_IDX2);
            dbClient_->execSqlSync(CREATE_ERROR_EVENT_PG_IDX3);
            dbClient_->execSqlSync(CREATE_ERROR_AGG_HOUR_PG);
            dbClient_->execSqlSync(CREATE_REQUEST_AGG_HOUR_PG);
        }
        LOG_INFO << "[错误统计数据库] 表结构检查完成";
    } catch (const std::exception& e) {
        LOG_ERROR << "[错误统计数据库] 创建表失败：" << e.what();
    }
}

std::string ErrorStatsDbManager::truncateBucket(const std::chrono::system_clock::time_point& tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm = *std::gmtime(&tt);
    tm.tm_min = 0;
    tm.tm_sec = 0;
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

// ============================================================================
// 辅助函数：转义 SQL 字符串值（防止 SQL 注入）
// ============================================================================
static std::string escapeSqlString(const std::string& str) {
    std::string result;
    result.reserve(str.size() * 2 + 2);
    result += "'";
    for (char c : str) {
        if (c == '\'') {
            result += "''";
        } else if (c == '\0') {

        } else {
            result += c;
        }
    }
    result += "'";
    return result;
}

// ============================================================================
// 优化：批量插入事件（使用事务）
// ============================================================================
bool ErrorStatsDbManager::insertEvents(const std::vector<ErrorEvent>& events) {
    if (events.empty()) return true;
    try {
        // 使用事务包裹批量插入，显著提高性能
        auto trans = dbClient_->newTransaction();
        
        for (const auto& ev : events) {
            auto tt = std::chrono::system_clock::to_time_t(ev.ts);
            char tsBuf[32];
            std::strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%d %H:%M:%S", std::gmtime(&tt));
            Json::StreamWriterBuilder wb;
            std::string detailStr = Json::writeString(wb, ev.detailJson);
            trans->execSqlSync(
                "INSERT INTO error_event (ts, severity, domain, type, provider, model, client_type, "
                "api_kind, stream, http_status, request_id, response_id, tool_name, message, detail_json, raw_snippet) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16)",
                std::string(tsBuf), ErrorEvent::severityToString(ev.severity), ErrorEvent::domainToString(ev.domain),
                ev.type, ev.provider, ev.model, ev.clientType, ev.apiKind, ev.stream,
                ev.httpStatus, ev.requestId, ev.responseId, ev.toolName, ev.message, detailStr, ev.rawSnippet
            );
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "[错误统计数据库] 写入明细事件失败：" << e.what();
        return false;
    }
}

// ============================================================================
// 优化：批量更新错误聚合（使用事务）
// ============================================================================
bool ErrorStatsDbManager::upsertErrorAggHour(const std::vector<ErrorEvent>& events) {
    if (events.empty()) return true;
    try {
        // 使用事务包裹批量操作
        auto trans = dbClient_->newTransaction();
        
        for (const auto& ev : events) {
            std::string bucket = truncateBucket(ev.ts);
            auto tt = std::chrono::system_clock::to_time_t(ev.ts);
            char tsBuf[32];
            std::strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%d %H:%M:%S", std::gmtime(&tt));
            if (dbType_ == DbType::PostgreSQL) {
                trans->execSqlSync(
                    "INSERT INTO error_agg_hour (bucket_start, severity, domain, type, provider, model, "
                    "client_type, api_kind, stream, http_status, count, last_event_ts) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, 1, $11) "
                    "ON CONFLICT (bucket_start, severity, domain, type, provider, model, client_type, api_kind, stream, http_status) "
                    "DO UPDATE SET count = error_agg_hour.count + 1, last_event_ts = $11",
                    bucket, ErrorEvent::severityToString(ev.severity), ErrorEvent::domainToString(ev.domain), ev.type,
                    ev.provider, ev.model, ev.clientType, ev.apiKind, ev.stream, ev.httpStatus, std::string(tsBuf)
                );
            } else {
                trans->execSqlSync(
                    "INSERT OR REPLACE INTO error_agg_hour (bucket_start, severity, domain, type, provider, model, "
                    "client_type, api_kind, stream, http_status, count, last_event_ts) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, "
                    "COALESCE((SELECT count FROM error_agg_hour WHERE bucket_start=$1 AND severity=$2 AND domain=$3 "
                    "AND type=$4 AND provider=$5 AND model=$6 AND client_type=$7 AND api_kind=$8 AND stream=$9 AND http_status=$10), 0) + 1, $11)",
                    bucket, ErrorEvent::severityToString(ev.severity), ErrorEvent::domainToString(ev.domain), ev.type,
                    ev.provider, ev.model, ev.clientType, ev.apiKind, ev.stream ? 1 : 0, ev.httpStatus, std::string(tsBuf)
                );
            }
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "[错误统计数据库] 写入错误聚合数据失败：" << e.what();
        return false;
    }
}

bool ErrorStatsDbManager::upsertRequestAggHour(const RequestAggData& data) {
    try {
        std::string bucket = truncateBucket(data.ts);
        auto tt = std::chrono::system_clock::to_time_t(data.ts);
        char tsBuf[32];
        std::strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%d %H:%M:%S", std::gmtime(&tt));
        if (dbType_ == DbType::PostgreSQL) {
            dbClient_->execSqlSync(
                "INSERT INTO request_agg_hour (bucket_start, provider, model, client_type, api_kind, stream, http_status, count, last_request_ts) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, 1, $8) "
                "ON CONFLICT (bucket_start, provider, model, client_type, api_kind, stream, http_status) "
                "DO UPDATE SET count = request_agg_hour.count + 1, last_request_ts = $8",
                bucket, data.provider, data.model, data.clientType, data.apiKind, data.stream, data.httpStatus, std::string(tsBuf)
            );
        } else {
            dbClient_->execSqlSync(
                "INSERT OR REPLACE INTO request_agg_hour (bucket_start, provider, model, client_type, api_kind, stream, http_status, count, last_request_ts) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, "
                "COALESCE((SELECT count FROM request_agg_hour WHERE bucket_start=$1 AND provider=$2 AND model=$3 "
                "AND client_type=$4 AND api_kind=$5 AND stream=$6 AND http_status=$7), 0) + 1, $8)",
                bucket, data.provider, data.model, data.clientType, data.apiKind, data.stream ? 1 : 0, data.httpStatus, std::string(tsBuf)
            );
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "[错误统计数据库] 写入请求聚合数据失败：" << e.what();
        return false;
    }
}

// ============================================================================
// 修复：查询错误时间序列 参数绑定
// 原问题：动态添加过滤条件但只传递了前两个参数
// 解决方案：使用字符串拼接构建完整 SQL（值已转义）
// ============================================================================
std::vector<AggBucket> ErrorStatsDbManager::queryErrorSeries(const QueryParams& params) {
    std::vector<AggBucket> result;
    try {
        std::ostringstream sql;
        sql << "SELECT bucket_start, SUM(count) as total FROM error_agg_hour WHERE bucket_start >= $1 AND bucket_start <= $2";
        
        // 使用字符串拼接方式添加过滤条件（值已转义，安全）
        if (!params.severity.empty()) {
            sql << " AND severity = " << escapeSqlString(params.severity);
        }
        if (!params.domain.empty()) {
            sql << " AND domain = " << escapeSqlString(params.domain);
        }
        if (!params.type.empty()) {
            sql << " AND type = " << escapeSqlString(params.type);
        }
        if (!params.provider.empty()) {
            sql << " AND provider = " << escapeSqlString(params.provider);
        }
        if (!params.model.empty()) {
            sql << " AND model = " << escapeSqlString(params.model);
        }
        if (!params.clientType.empty()) {
            sql << " AND client_type = " << escapeSqlString(params.clientType);
        }
        sql << " GROUP BY bucket_start ORDER BY bucket_start";
        
        auto res = dbClient_->execSqlSync(sql.str(), params.from, params.to);
        for (const auto& row : res) {
            AggBucket b;
            b.bucketStart = row["bucket_start"].as<std::string>();
            b.count = row["total"].as<int64_t>();
            result.push_back(b);
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "[错误统计数据库] 查询错误时间序列失败：" << e.what();
    }
    return result;
}

std::vector<AggBucket> ErrorStatsDbManager::queryRequestSeries(const QueryParams& params) {
    std::vector<AggBucket> result;
    try {
        std::ostringstream sql;
        sql << "SELECT bucket_start, SUM(count) as total FROM request_agg_hour WHERE bucket_start >= $1 AND bucket_start <= $2";
        
        // 添加可选过滤条件
        if (!params.provider.empty()) {
            sql << " AND provider = " << escapeSqlString(params.provider);
        }
        if (!params.model.empty()) {
            sql << " AND model = " << escapeSqlString(params.model);
        }
        if (!params.clientType.empty()) {
            sql << " AND client_type = " << escapeSqlString(params.clientType);
        }
        sql << " GROUP BY bucket_start ORDER BY bucket_start";
        
        auto res = dbClient_->execSqlSync(sql.str(), params.from, params.to);
        for (const auto& row : res) {
            AggBucket b;
            b.bucketStart = row["bucket_start"].as<std::string>();
            b.count = row["total"].as<int64_t>();
            result.push_back(b);
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "[错误统计数据库] 查询请求时间序列失败：" << e.what();
    }
    return result;
}

// ============================================================================
// 修复：查询错误事件 参数绑定
// ============================================================================
std::vector<ErrorEventRecord> ErrorStatsDbManager::queryEvents(const QueryParams& params, int limit, int offset) {
    std::vector<ErrorEventRecord> result;
    try {
        std::ostringstream sql;
        sql << "SELECT id, ts, severity, domain, type, provider, model, client_type, api_kind, stream, http_status, "
            << "request_id, response_id, tool_name, message, detail_json, raw_snippet FROM error_event WHERE ts >= $1 AND ts <= $2";
        
        // 使用字符串拼接方式添加过滤条件（值已转义，安全）
        if (!params.severity.empty()) {
            sql << " AND severity = " << escapeSqlString(params.severity);
        }
        if (!params.domain.empty()) {
            sql << " AND domain = " << escapeSqlString(params.domain);
        }
        if (!params.type.empty()) {
            sql << " AND type = " << escapeSqlString(params.type);
        }
        if (!params.provider.empty()) {
            sql << " AND provider = " << escapeSqlString(params.provider);
        }
        if (!params.model.empty()) {
            sql << " AND model = " << escapeSqlString(params.model);
        }
        if (!params.clientType.empty()) {
            sql << " AND client_type = " << escapeSqlString(params.clientType);
        }
        sql << " ORDER BY ts DESC LIMIT " << limit << " OFFSET " << offset;
        
        auto res = dbClient_->execSqlSync(sql.str(), params.from, params.to);
        for (const auto& row : res) {
            ErrorEventRecord rec;
            rec.id = row["id"].as<int64_t>();
            rec.ts = row["ts"].as<std::string>();
            rec.severity = row["severity"].as<std::string>();
            rec.domain = row["domain"].as<std::string>();
            rec.type = row["type"].as<std::string>();
            rec.provider = row["provider"].isNull() ? "" : row["provider"].as<std::string>();
            rec.model = row["model"].isNull() ? "" : row["model"].as<std::string>();
            rec.clientType = row["client_type"].isNull() ? "" : row["client_type"].as<std::string>();
            rec.apiKind = row["api_kind"].isNull() ? "" : row["api_kind"].as<std::string>();
            rec.stream = row["stream"].isNull() ? false : row["stream"].as<bool>();
            rec.httpStatus = row["http_status"].isNull() ? 0 : row["http_status"].as<int>();
            rec.requestId = row["request_id"].isNull() ? "" : row["request_id"].as<std::string>();
            rec.message = row["message"].isNull() ? "" : row["message"].as<std::string>();
            rec.detailJson = row["detail_json"].isNull() ? "" : row["detail_json"].as<std::string>();
            result.push_back(rec);
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "[错误统计数据库] 查询错误事件失败：" << e.what();
    }
    return result;
}

std::optional<ErrorEventRecord> ErrorStatsDbManager::queryEventById(int64_t id) {
    try {
        auto res = dbClient_->execSqlSync(
            "SELECT id, ts, severity, domain, type, provider, model, client_type, api_kind, stream, http_status, "
            "request_id, response_id, tool_name, message, detail_json, raw_snippet FROM error_event WHERE id = $1", id);
        if (res.empty()) return std::nullopt;
        const auto& row = res[0];
        ErrorEventRecord rec;
        rec.id = row["id"].as<int64_t>();
        rec.ts = row["ts"].as<std::string>();
        rec.severity = row["severity"].as<std::string>();
        rec.domain = row["domain"].as<std::string>();
        rec.type = row["type"].as<std::string>();
        rec.provider = row["provider"].isNull() ? "" : row["provider"].as<std::string>();
        rec.model = row["model"].isNull() ? "" : row["model"].as<std::string>();
        rec.clientType = row["client_type"].isNull() ? "" : row["client_type"].as<std::string>();
        rec.apiKind = row["api_kind"].isNull() ? "" : row["api_kind"].as<std::string>();
        rec.stream = row["stream"].isNull() ? false : row["stream"].as<bool>();
        rec.httpStatus = row["http_status"].isNull() ? 0 : row["http_status"].as<int>();
        rec.requestId = row["request_id"].isNull() ? "" : row["request_id"].as<std::string>();
        rec.message = row["message"].isNull() ? "" : row["message"].as<std::string>();
        rec.detailJson = row["detail_json"].isNull() ? "" : row["detail_json"].as<std::string>();
        rec.rawSnippet = row["raw_snippet"].isNull() ? "" : row["raw_snippet"].as<std::string>();
        return rec;
    } catch (const std::exception& e) {
        LOG_ERROR << "[错误统计数据库] 按ID查询错误事件失败：" << e.what();
        return std::nullopt;
    }
}

int ErrorStatsDbManager::cleanupOldEvents(int retentionDays) {
    try {
        std::string sql;
        if (dbType_ == DbType::PostgreSQL) {
            sql = "DELETE FROM error_event WHERE ts < NOW() - INTERVAL '" + std::to_string(retentionDays) + " days'";
        } else {
            sql = "DELETE FROM error_event WHERE ts < datetime('now', '-" + std::to_string(retentionDays) + " days')";
        }
        auto res = dbClient_->execSqlSync(sql);
        return static_cast<int>(res.affectedRows());
    } catch (const std::exception& e) {
        LOG_ERROR << "[错误统计数据库] 清理过期明细事件失败：" << e.what();
        return 0;
    }
}

int ErrorStatsDbManager::cleanupOldAgg(int retentionDays) {
    try {
        std::string sql1, sql2;
        if (dbType_ == DbType::PostgreSQL) {
            sql1 = "DELETE FROM error_agg_hour WHERE bucket_start < NOW() - INTERVAL '" + std::to_string(retentionDays) + " days'";
            sql2 = "DELETE FROM request_agg_hour WHERE bucket_start < NOW() - INTERVAL '" + std::to_string(retentionDays) + " days'";
        } else {
            sql1 = "DELETE FROM error_agg_hour WHERE bucket_start < datetime('now', '-" + std::to_string(retentionDays) + " days')";
            sql2 = "DELETE FROM request_agg_hour WHERE bucket_start < datetime('now', '-" + std::to_string(retentionDays) + " days')";
        }
        auto res1 = dbClient_->execSqlSync(sql1);
        auto res2 = dbClient_->execSqlSync(sql2);
        return static_cast<int>(res1.affectedRows() + res2.affectedRows());
    } catch (const std::exception& e) {
        LOG_ERROR << "[错误统计数据库] 清理过期聚合数据失败：" << e.what();
        return 0;
    }
}

} // 命名空间结束
