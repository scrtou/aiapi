#include <dbManager/chaynsThread/chaynsThreadDbManager.h>
#include <domain/port/IBackgroundExecutor.h>
#include <algorithm>
#include <sstream>

namespace {
constexpr const char* kLogTag = "[chayns线程台账]";
}

void chaynsThreadDbManager::initialize()
{
    if (initialized_) return;
    initialized_ = true;
    try {
        dbClient_ = drogon::app().getDbClient("aichatpg");
        detectDbType();
        enabled_ = (dbClient_ != nullptr);
    } catch (const std::exception& ex) {
        LOG_WARN << kLogTag << " 获取数据库客户端失败，线程台账降级为纯内存: " << ex.what();
        enabled_ = false;
    }
}

void chaynsThreadDbManager::detectDbType()
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

std::string chaynsThreadDbManager::getCreateThreadTableSql() const
{
    switch (dbType_) {
        case DbType::SQLite3:
            return R"(
                CREATE TABLE IF NOT EXISTS chaynsa_thread (
                    thread_id TEXT PRIMARY KEY,
                    session_id TEXT DEFAULT '',
                    account_user_name TEXT DEFAULT '',
                    origin TEXT DEFAULT '',
                    referer TEXT DEFAULT '',
                    created_at INTEGER DEFAULT 0,
                    last_active_at INTEGER DEFAULT 0,
                    delete_attempts INTEGER DEFAULT 0
                );
            )";
        case DbType::MySQL:
            return R"(
                CREATE TABLE IF NOT EXISTS chaynsa_thread (
                    thread_id VARCHAR(191) PRIMARY KEY,
                    session_id VARCHAR(191) DEFAULT '',
                    account_user_name VARCHAR(191) DEFAULT '',
                    origin VARCHAR(255) DEFAULT '',
                    referer VARCHAR(255) DEFAULT '',
                    created_at BIGINT DEFAULT 0,
                    last_active_at BIGINT DEFAULT 0,
                    delete_attempts INT DEFAULT 0
                ) ENGINE=InnoDB;
            )";
        case DbType::PostgreSQL:
        default:
            return R"(
                CREATE TABLE IF NOT EXISTS chaynsa_thread (
                    thread_id VARCHAR(191) PRIMARY KEY,
                    session_id VARCHAR(191) DEFAULT '',
                    account_user_name VARCHAR(191) DEFAULT '',
                    origin VARCHAR(255) DEFAULT '',
                    referer VARCHAR(255) DEFAULT '',
                    created_at BIGINT DEFAULT 0,
                    last_active_at BIGINT DEFAULT 0,
                    delete_attempts INTEGER DEFAULT 0
                );
            )";
    }
}

std::string chaynsThreadDbManager::getCreateThreadActiveIndexSql() const
{
    // reaper 每轮按 last_active_at < cutoff 捞取待回收行，是最热的扫描路径。
    return "CREATE INDEX IF NOT EXISTS idx_chaynsa_thread_last_active "
           "ON chaynsa_thread (last_active_at);";
}

std::string chaynsThreadDbManager::getCreateThreadSessionIndexSql() const
{
    // detach / updateSessionId / touch 全部按 session_id 定位，且允许空串（已解绑）。
    return "CREATE INDEX IF NOT EXISTS idx_chaynsa_thread_session "
           "ON chaynsa_thread (session_id);";
}

bool chaynsThreadDbManager::ensureTable(std::string* errorMessage)
{
    if (!dbClient_) {
        enabled_ = false;
        if (errorMessage) *errorMessage = "未获取到数据库客户端";
        return false;
    }

    try {
        dbClient_->execSqlSync(getCreateThreadTableSql());
        // 索引创建失败不致命（MySQL 5.7 不支持 IF NOT EXISTS on index）
        try { dbClient_->execSqlSync(getCreateThreadActiveIndexSql()); } catch (...) {}
        try { dbClient_->execSqlSync(getCreateThreadSessionIndexSql()); } catch (...) {}
        enabled_ = true;
        LOG_INFO << kLogTag << " 建表完成: chaynsa_thread";
        return true;
    } catch (const std::exception& ex) {
        enabled_ = false;
        if (errorMessage) *errorMessage = std::string("创建 chaynsa_thread 失败: ") + ex.what();
        LOG_ERROR << kLogTag << " 建表失败，线程台账降级为纯内存: " << ex.what();
        return false;
    }
}

// ========================= 行映射 =========================

chaynsThreadDbManager::ThreadRow chaynsThreadDbManager::rowFromRecord(const drogon::orm::Row& r)
{
    ThreadRow row;
    row.threadId        = r["thread_id"].as<std::string>();
    row.sessionId       = r["session_id"].isNull() ? "" : r["session_id"].as<std::string>();
    row.accountUserName = r["account_user_name"].isNull() ? "" : r["account_user_name"].as<std::string>();
    row.origin          = r["origin"].isNull() ? "" : r["origin"].as<std::string>();
    row.referer         = r["referer"].isNull() ? "" : r["referer"].as<std::string>();
    row.createdAt       = r["created_at"].isNull() ? 0 : r["created_at"].as<int64_t>();
    row.lastActiveAt    = r["last_active_at"].isNull() ? 0 : r["last_active_at"].as<int64_t>();
    row.deleteAttempts  = r["delete_attempts"].isNull() ? 0 : r["delete_attempts"].as<int>();
    return row;
}

// ========================= CRUD =========================

bool chaynsThreadDbManager::upsertThread(const ThreadRow& row, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || row.threadId.empty()) return false;

    try {
        // 沿用「先查后写」通用路径，规避 PG/MySQL/SQLite 三套 upsert 语法差异。
        auto existing = dbClient_->execSqlSync(
            "select thread_id from chaynsa_thread where thread_id=$1 limit 1",
            row.threadId);
        if (existing.empty()) {
            dbClient_->execSqlSync(
                "insert into chaynsa_thread(thread_id, session_id, account_user_name, origin, referer, "
                "created_at, last_active_at, delete_attempts) values($1, $2, $3, $4, $5, $6, $7, $8)",
                row.threadId, row.sessionId, row.accountUserName, row.origin, row.referer,
                static_cast<int64_t>(row.createdAt), static_cast<int64_t>(row.lastActiveAt),
                row.deleteAttempts);
        } else {
            dbClient_->execSqlSync(
                "update chaynsa_thread set session_id=$1, account_user_name=$2, origin=$3, referer=$4, "
                "last_active_at=$5 where thread_id=$6",
                row.sessionId, row.accountUserName, row.origin, row.referer,
                static_cast<int64_t>(row.lastActiveAt), row.threadId);
        }
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("写入线程台账失败: ") + ex.what();
        LOG_WARN << kLogTag << " upsertThread 失败(降级忽略): " << ex.what();
        return false;
    }
}

bool chaynsThreadDbManager::detachThreadBySessionId(const std::string& sessionId, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || sessionId.empty()) return false;
    try {
        // 关键：只解绑不删行。上游 thread 仍然存在，必须留给 reaper 异步回收，
        // 否则请求路径要么阻塞在 HTTP 删除上，要么直接泄漏上游资源。
        dbClient_->execSqlSync(
            "update chaynsa_thread set session_id='' where session_id=$1",
            sessionId);
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("解绑线程失败: ") + ex.what();
        LOG_WARN << kLogTag << " detachThreadBySessionId 失败(降级忽略): " << ex.what();
        return false;
    }
}

bool chaynsThreadDbManager::updateThreadSessionId(const std::string& oldSessionId,
                                                  const std::string& newSessionId,
                                                  std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || oldSessionId.empty() || newSessionId.empty()) return false;
    try {
        dbClient_->execSqlSync(
            "update chaynsa_thread set session_id=$1, last_active_at=$2 where session_id=$3",
            newSessionId, static_cast<int64_t>(time(nullptr)), oldSessionId);
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("轮转线程会话键失败: ") + ex.what();
        LOG_WARN << kLogTag << " updateThreadSessionId 失败(降级忽略): " << ex.what();
        return false;
    }
}

bool chaynsThreadDbManager::touchThreadBySessionId(const std::string& sessionId,
                                                   int64_t nowEpochSeconds,
                                                   std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || sessionId.empty()) return false;
    try {
        dbClient_->execSqlSync(
            "update chaynsa_thread set last_active_at=$1 where session_id=$2",
            nowEpochSeconds, sessionId);
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("刷新线程活跃时间失败: ") + ex.what();
        LOG_WARN << kLogTag << " touchThreadBySessionId 失败(降级忽略): " << ex.what();
        return false;
    }
}

std::vector<chaynsThreadDbManager::ThreadRow> chaynsThreadDbManager::loadThreadsOlderThan(
    int64_t cutoffEpochSeconds, int limit, std::string* errorMessage)
{
    std::vector<ThreadRow> rows;
    if (!enabled_ || !dbClient_ || limit <= 0) return rows;
    try {
        // 注意 last_active_at > 0 的保护：0 表示时间戳缺失，不能当成"极旧"误删刚建的线程。
        auto result = dbClient_->execSqlSync(
            "select thread_id, session_id, account_user_name, origin, referer, "
            "created_at, last_active_at, delete_attempts from chaynsa_thread "
            "where last_active_at > 0 and last_active_at < $1 "
            "order by last_active_at asc limit $2",
            cutoffEpochSeconds, limit);
        rows.reserve(result.size());
        for (const auto& r : result) {
            rows.push_back(rowFromRecord(r));
        }
        return rows;
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("捞取待回收线程失败: ") + ex.what();
        LOG_WARN << kLogTag << " loadThreadsOlderThan 失败(本轮跳过): " << ex.what();
        return rows;
    }
}

std::optional<chaynsThreadDbManager::ThreadRow> chaynsThreadDbManager::loadThreadBySessionId(
    const std::string& sessionId, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || sessionId.empty()) return std::nullopt;
    try {
        auto result = dbClient_->execSqlSync(
            "select thread_id, session_id, account_user_name, origin, referer, "
            "created_at, last_active_at, delete_attempts from chaynsa_thread "
            "where session_id=$1 limit 1",
            sessionId);
        if (result.empty()) return std::nullopt;
        return rowFromRecord(result[0]);
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("读取线程台账失败: ") + ex.what();
        LOG_WARN << kLogTag << " loadThreadBySessionId 失败(降级为未命中): " << ex.what();
        return std::nullopt;
    }
}

bool chaynsThreadDbManager::deleteThread(const std::string& threadId, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || threadId.empty()) return false;
    try {
        dbClient_->execSqlSync("delete from chaynsa_thread where thread_id=$1", threadId);
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("删除线程台账失败: ") + ex.what();
        LOG_WARN << kLogTag << " deleteThread 失败(降级忽略): " << ex.what();
        return false;
    }
}

bool chaynsThreadDbManager::deleteThreads(const std::vector<std::string>& threadIds, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || threadIds.empty()) return false;
    bool allOk = true;
    for (const auto& id : threadIds) {
        if (!deleteThread(id, errorMessage)) allOk = false;
    }
    return allOk;
}

int chaynsThreadDbManager::bumpDeleteAttempts(const std::string& threadId, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || threadId.empty()) return -1;
    try {
        dbClient_->execSqlSync(
            "update chaynsa_thread set delete_attempts = delete_attempts + 1 where thread_id=$1",
            threadId);
        auto result = dbClient_->execSqlSync(
            "select delete_attempts from chaynsa_thread where thread_id=$1 limit 1", threadId);
        if (result.empty()) return -1;
        return result[0]["delete_attempts"].isNull() ? 0 : result[0]["delete_attempts"].as<int>();
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("累加删除重试计数失败: ") + ex.what();
        LOG_WARN << kLogTag << " bumpDeleteAttempts 失败(降级忽略): " << ex.what();
        return -1;
    }
}

int chaynsThreadDbManager::purgeExhaustedThreads(int maxAttempts, std::string* errorMessage)
{
    if (!enabled_ || !dbClient_ || maxAttempts <= 0) return -1;
    try {
        // 上游删不掉的行不能无限占位反复被捞出，超阈值后按孤儿丢弃（仅本地台账，上游残留由人工/上游 TTL 兜底）。
        auto result = dbClient_->execSqlSync(
            "delete from chaynsa_thread where delete_attempts >= $1", maxAttempts);
        return static_cast<int>(result.affectedRows());
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("清理重试耗尽线程失败: ") + ex.what();
        LOG_WARN << kLogTag << " purgeExhaustedThreads 失败(降级忽略): " << ex.what();
        return -1;
    }
}

// ========================= 异步写穿入口（injected executor）=========================

void chaynsThreadDbManager::submitWrite(const char* taskName, std::function<void()> task)
{
    if (!executor_) {
        // A missing executor is a wiring error, not a reason to revive the
        // old global queue.  Preserve the request-path nonblocking contract
        // and make the lost write explicit in logs.
        LOG_ERROR << "[chayns线程DB] executor 未注入，数据未落盘：" << taskName;
        return;
    }
    const auto result = executor_->submit(taskName, std::move(task));
    if (result != TaskSubmitResult::Accepted) {
        LOG_ERROR << "[chayns线程DB] 异步写穿任务入队被拒(" << toString(result)
                  << ")，数据未落盘：" << taskName;
    }
}

void chaynsThreadDbManager::asyncUpsertThread(const ThreadRow& row)
{
    if (!enabled_ || row.threadId.empty()) return;
    const auto self = weak_from_this().lock();
    if (!self) {
        LOG_ERROR << "[chayns线程DB] 未由 shared_ptr 持有，数据未落盘：chaynsThread.upsert";
        return;
    }
    submitWrite("chaynsThread.upsert", [self, row]() {
        self->upsertThread(row);
    });
}

void chaynsThreadDbManager::asyncDetachThreadBySessionId(const std::string& sessionId)
{
    if (!enabled_ || sessionId.empty()) return;
    const auto self = weak_from_this().lock();
    if (!self) {
        LOG_ERROR << "[chayns线程DB] 未由 shared_ptr 持有，数据未落盘：chaynsThread.detach";
        return;
    }
    submitWrite("chaynsThread.detach", [self, sessionId]() {
        self->detachThreadBySessionId(sessionId);
    });
}

void chaynsThreadDbManager::asyncUpdateThreadSessionId(const std::string& oldSessionId,
                                                       const std::string& newSessionId)
{
    if (!enabled_ || oldSessionId.empty() || newSessionId.empty()) return;
    const auto self = weak_from_this().lock();
    if (!self) {
        LOG_ERROR << "[chayns线程DB] 未由 shared_ptr 持有，数据未落盘：chaynsThread.rotateSession";
        return;
    }
    submitWrite("chaynsThread.rotateSession", [self, oldSessionId, newSessionId]() {
        self->updateThreadSessionId(oldSessionId, newSessionId);
    });
}

void chaynsThreadDbManager::asyncTouchThreadBySessionId(const std::string& sessionId, int64_t nowEpochSeconds)
{
    if (!enabled_ || sessionId.empty()) return;
    const auto self = weak_from_this().lock();
    if (!self) {
        LOG_ERROR << "[chayns线程DB] 未由 shared_ptr 持有，数据未落盘：chaynsThread.touch";
        return;
    }
    submitWrite("chaynsThread.touch", [self, sessionId, nowEpochSeconds]() {
        self->touchThreadBySessionId(sessionId, nowEpochSeconds);
    });
}

void chaynsThreadDbManager::asyncDeleteThread(const std::string& threadId)
{
    if (!enabled_ || threadId.empty()) return;
    const auto self = weak_from_this().lock();
    if (!self) {
        LOG_ERROR << "[chayns线程DB] 未由 shared_ptr 持有，数据未落盘：chaynsThread.delete";
        return;
    }
    submitWrite("chaynsThread.delete", [self, threadId]() {
        self->deleteThread(threadId);
    });
}
