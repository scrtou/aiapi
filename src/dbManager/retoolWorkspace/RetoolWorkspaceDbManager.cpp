#include "RetoolWorkspaceDbManager.h"

#include <algorithm>
#include <cctype>

namespace
{
RetoolWorkspaceInfo rowToInfo(const drogon::orm::Row& row)
{
    RetoolWorkspaceInfo info;
    info.workspaceId = row["workspace_id"].as<std::string>();
    info.email = row["email"].as<std::string>();
    info.password = row["password"].as<std::string>();
    info.mailProvider = row["mail_provider"].as<std::string>();
    info.mailAccountId = row["mail_account_id"].as<std::string>();
    info.baseUrl = row["base_url"].as<std::string>();
    info.subdomain = row["subdomain"].as<std::string>();
    info.accessToken = row["access_token"].as<std::string>();
    info.xsrfToken = row["xsrf_token"].as<std::string>();
    info.extraCookiesJson = row["extra_cookies_json"].as<std::string>();
    info.openaiResourceUuid = row["openai_resource_uuid"].as<std::string>();
    info.openaiResourceName = row["openai_resource_name"].as<std::string>();
    info.anthropicResourceUuid = row["anthropic_resource_uuid"].as<std::string>();
    info.anthropicResourceName = row["anthropic_resource_name"].as<std::string>();
    info.workflowId = row["workflow_id"].as<std::string>();
    info.workflowApiKey = row["workflow_api_key"].as<std::string>();
    info.agentId = row["agent_id"].as<std::string>();
    info.status = row["status"].as<std::string>();
    info.verifyStatus = row["verify_status"].as<std::string>();
    info.lastVerifyAt = row["last_verify_at"].isNull() ? "" : row["last_verify_at"].as<std::string>();
    info.lastUsedAt = row["last_used_at"].isNull() ? "" : row["last_used_at"].as<std::string>();
    info.inUseCount = row["in_use_count"].isNull() ? 0 : row["in_use_count"].as<int>();
    info.notesJson = row["notes_json"].as<std::string>();
    info.createdAt = row["created_at"].isNull() ? "" : row["created_at"].as<std::string>();
    info.updatedAt = row["updated_at"].isNull() ? "" : row["updated_at"].as<std::string>();
    return info;
}

bool isDuplicateColumnError(std::string message)
{
    std::transform(message.begin(), message.end(), message.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return message.find("duplicate column") != std::string::npos ||
           message.find("already exists") != std::string::npos;
}
}  // namespace

void RetoolWorkspaceDbManager::detectDbType()
{
    auto customConfig = drogon::app().getCustomConfig();
    std::string dbTypeStr = "postgresql";
    if (customConfig.isMember("dbtype"))
    {
        dbTypeStr = customConfig["dbtype"].asString();
    }
    std::transform(dbTypeStr.begin(), dbTypeStr.end(), dbTypeStr.begin(), ::tolower);
    if (dbTypeStr == "sqlite3" || dbTypeStr == "sqlite")
    {
        dbType_ = DbType::SQLite3;
    }
    else if (dbTypeStr == "mysql" || dbTypeStr == "mariadb")
    {
        dbType_ = DbType::MySQL;
    }
    else
    {
        dbType_ = DbType::PostgreSQL;
    }
}

std::string RetoolWorkspaceDbManager::createTableSql() const
{
    switch (dbType_)
    {
        case DbType::SQLite3:
            return R"(
                CREATE TABLE IF NOT EXISTS retool_workspace (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    workspace_id TEXT UNIQUE NOT NULL,
                    email TEXT NOT NULL,
                    password TEXT NOT NULL,
                    mail_provider TEXT DEFAULT '',
                    mail_account_id TEXT DEFAULT '',
                    base_url TEXT NOT NULL,
                    subdomain TEXT NOT NULL,
                    access_token TEXT DEFAULT '',
                    xsrf_token TEXT DEFAULT '',
                    extra_cookies_json TEXT DEFAULT '{}',
                    openai_resource_uuid TEXT DEFAULT '',
                    openai_resource_name TEXT DEFAULT '',
                    anthropic_resource_uuid TEXT DEFAULT '',
                    anthropic_resource_name TEXT DEFAULT '',
                    workflow_id TEXT DEFAULT '',
                    workflow_api_key TEXT DEFAULT '',
                    agent_id TEXT DEFAULT '',
                    status TEXT DEFAULT 'provisioning',
                    verify_status TEXT DEFAULT 'unknown',
                    last_verify_at TEXT DEFAULT '',
                    last_used_at TEXT DEFAULT '',
                    in_use_count INTEGER DEFAULT 0,
                    notes_json TEXT DEFAULT '{}',
                    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
                );
            )";
        case DbType::MySQL:
            return R"(
                CREATE TABLE IF NOT EXISTS retool_workspace (
                    id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
                    workspace_id VARCHAR(255) NOT NULL UNIQUE,
                    email VARCHAR(255) NOT NULL,
                    password TEXT NOT NULL,
                    mail_provider VARCHAR(64) DEFAULT '',
                    mail_account_id VARCHAR(255) DEFAULT '',
                    base_url TEXT NOT NULL,
                    subdomain VARCHAR(255) NOT NULL,
                    access_token TEXT,
                    xsrf_token TEXT,
                    extra_cookies_json TEXT,
                    openai_resource_uuid VARCHAR(255) DEFAULT '',
                    openai_resource_name VARCHAR(255) DEFAULT '',
                    anthropic_resource_uuid VARCHAR(255) DEFAULT '',
                    anthropic_resource_name VARCHAR(255) DEFAULT '',
                    workflow_id VARCHAR(255) DEFAULT '',
                    workflow_api_key VARCHAR(255) DEFAULT '',
                    agent_id VARCHAR(255) DEFAULT '',
                    status VARCHAR(64) DEFAULT 'provisioning',
                    verify_status VARCHAR(64) DEFAULT 'unknown',
                    last_verify_at VARCHAR(64) DEFAULT '',
                    last_used_at VARCHAR(64) DEFAULT '',
                    in_use_count INT DEFAULT 0,
                    notes_json TEXT,
                    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
                ) ENGINE=InnoDB;
            )";
        case DbType::PostgreSQL:
        default:
            return R"(
                CREATE TABLE IF NOT EXISTS retool_workspace (
                    id SERIAL PRIMARY KEY,
                    workspace_id VARCHAR(255) UNIQUE NOT NULL,
                    email VARCHAR(255) NOT NULL,
                    password TEXT NOT NULL,
                    mail_provider VARCHAR(64) DEFAULT '',
                    mail_account_id VARCHAR(255) DEFAULT '',
                    base_url TEXT NOT NULL,
                    subdomain VARCHAR(255) NOT NULL,
                    access_token TEXT DEFAULT '',
                    xsrf_token TEXT DEFAULT '',
                    extra_cookies_json TEXT DEFAULT '{}',
                    openai_resource_uuid VARCHAR(255) DEFAULT '',
                    openai_resource_name VARCHAR(255) DEFAULT '',
                    anthropic_resource_uuid VARCHAR(255) DEFAULT '',
                    anthropic_resource_name VARCHAR(255) DEFAULT '',
                    workflow_id VARCHAR(255) DEFAULT '',
                    workflow_api_key VARCHAR(255) DEFAULT '',
                    agent_id VARCHAR(255) DEFAULT '',
                    status VARCHAR(64) DEFAULT 'provisioning',
                    verify_status VARCHAR(64) DEFAULT 'unknown',
                    last_verify_at VARCHAR(64) DEFAULT '',
                    last_used_at VARCHAR(64) DEFAULT '',
                    in_use_count INTEGER DEFAULT 0,
                    notes_json TEXT DEFAULT '{}',
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                );
            )";
    }
}

bool RetoolWorkspaceDbManager::ensureTable(std::string* errorMessage)
{
    try
    {
        dbClient_->execSqlSync(createTableSql());
        return ensureColumns(errorMessage);
    }
    catch (const std::exception& ex)
    {
        if (errorMessage) *errorMessage = std::string("创建 retool_workspace 表失败: ") + ex.what();
        return false;
    }
}

bool RetoolWorkspaceDbManager::ensureColumns(std::string* errorMessage)
{
    const std::vector<std::string> statements = [&]() {
        switch (dbType_)
        {
            case DbType::SQLite3:
                return std::vector<std::string>{
                    "alter table retool_workspace add column last_used_at TEXT DEFAULT ''",
                    "alter table retool_workspace add column in_use_count INTEGER DEFAULT 0",
                    "alter table retool_workspace add column anthropic_resource_uuid TEXT DEFAULT ''",
                    "alter table retool_workspace add column anthropic_resource_name TEXT DEFAULT ''",
                };
            case DbType::MySQL:
                return std::vector<std::string>{
                    "alter table retool_workspace add column last_used_at VARCHAR(64) DEFAULT ''",
                    "alter table retool_workspace add column in_use_count INT DEFAULT 0",
                    "alter table retool_workspace add column anthropic_resource_uuid VARCHAR(255) DEFAULT ''",
                    "alter table retool_workspace add column anthropic_resource_name VARCHAR(255) DEFAULT ''",
                };
            case DbType::PostgreSQL:
            default:
                return std::vector<std::string>{
                    "alter table retool_workspace add column last_used_at VARCHAR(64) DEFAULT ''",
                    "alter table retool_workspace add column in_use_count INTEGER DEFAULT 0",
                    "alter table retool_workspace add column anthropic_resource_uuid VARCHAR(255) DEFAULT ''",
                    "alter table retool_workspace add column anthropic_resource_name VARCHAR(255) DEFAULT ''",
                };
        }
    }();

    try
    {
        for (const auto& sql : statements)
        {
            try
            {
                dbClient_->execSqlSync(sql);
            }
            catch (const std::exception& ex)
            {
                if (!isDuplicateColumnError(ex.what()))
                {
                    if (errorMessage) *errorMessage = std::string("升级 retool_workspace 表失败: ") + ex.what();
                    return false;
                }
            }
        }
        return true;
    }
    catch (const std::exception& ex)
    {
        if (errorMessage) *errorMessage = std::string("升级 retool_workspace 表失败: ") + ex.what();
        return false;
    }
}

bool RetoolWorkspaceDbManager::upsertWorkspace(const RetoolWorkspaceInfo& info, std::string* errorMessage)
{
    if (!ensureTable(errorMessage))
    {
        return false;
    }

    try
    {
        auto exists = dbClient_->execSqlSync(
            "select workspace_id from retool_workspace where workspace_id=$1 limit 1", info.workspaceId);
        if (exists.empty())
        {
            dbClient_->execSqlSync(
                "insert into retool_workspace(workspace_id,email,password,mail_provider,mail_account_id,base_url,subdomain,"
                "access_token,xsrf_token,extra_cookies_json,openai_resource_uuid,openai_resource_name,anthropic_resource_uuid,anthropic_resource_name,workflow_id,"
                "workflow_api_key,agent_id,status,verify_status,last_verify_at,last_used_at,in_use_count,notes_json,created_at,updated_at) "
                "values($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21,$22,$23,CURRENT_TIMESTAMP,CURRENT_TIMESTAMP)",
                info.workspaceId,
                info.email,
                info.password,
                info.mailProvider,
                info.mailAccountId,
                info.baseUrl,
                info.subdomain,
                info.accessToken,
                info.xsrfToken,
                info.extraCookiesJson,
                info.openaiResourceUuid,
                info.openaiResourceName,
                info.anthropicResourceUuid,
                info.anthropicResourceName,
                info.workflowId,
                info.workflowApiKey,
                info.agentId,
                info.status,
                info.verifyStatus,
                info.lastVerifyAt,
                info.lastUsedAt,
                info.inUseCount,
                info.notesJson);
        }
        else
        {
            dbClient_->execSqlSync(
                "update retool_workspace set email=$1,password=$2,mail_provider=$3,mail_account_id=$4,base_url=$5,subdomain=$6,"
                "access_token=$7,xsrf_token=$8,extra_cookies_json=$9,openai_resource_uuid=$10,openai_resource_name=$11,anthropic_resource_uuid=$12,anthropic_resource_name=$13,"
                "workflow_id=$14,workflow_api_key=$15,agent_id=$16,status=$17,verify_status=$18,last_verify_at=$19,"
                "last_used_at=$20,in_use_count=$21,notes_json=$22,updated_at=CURRENT_TIMESTAMP where workspace_id=$23",
                info.email,
                info.password,
                info.mailProvider,
                info.mailAccountId,
                info.baseUrl,
                info.subdomain,
                info.accessToken,
                info.xsrfToken,
                info.extraCookiesJson,
                info.openaiResourceUuid,
                info.openaiResourceName,
                info.anthropicResourceUuid,
                info.anthropicResourceName,
                info.workflowId,
                info.workflowApiKey,
                info.agentId,
                info.status,
                info.verifyStatus,
                info.lastVerifyAt,
                info.lastUsedAt,
                info.inUseCount,
                info.notesJson,
                info.workspaceId);
        }
        return true;
    }
    catch (const std::exception& ex)
    {
        if (errorMessage) *errorMessage = std::string("保存 retool workspace 失败: ") + ex.what();
        return false;
    }
}

bool RetoolWorkspaceDbManager::deleteWorkspace(const std::string& workspaceId, std::string* errorMessage)
{
    if (!ensureTable(errorMessage))
    {
        return false;
    }
    try
    {
        dbClient_->execSqlSync(
            "delete from retool_workspace where workspace_id=$1",
            workspaceId);
        return true;
    }
    catch (const std::exception& ex)
    {
        if (errorMessage) *errorMessage = std::string("删除 retool workspace 失败: ") + ex.what();
        return false;
    }
}

std::optional<RetoolWorkspaceInfo> RetoolWorkspaceDbManager::getWorkspace(const std::string& workspaceId,
                                                                          std::string* errorMessage)
{
    if (!ensureTable(errorMessage))
    {
        return std::nullopt;
    }
    try
    {
        auto result = dbClient_->execSqlSync(
            "select workspace_id,email,password,mail_provider,mail_account_id,base_url,subdomain,access_token,xsrf_token,"
            "extra_cookies_json,openai_resource_uuid,openai_resource_name,anthropic_resource_uuid,anthropic_resource_name,workflow_id,workflow_api_key,agent_id,status,"
            "verify_status,last_verify_at,last_used_at,in_use_count,notes_json,created_at,updated_at from retool_workspace where workspace_id=$1 limit 1",
            workspaceId);
        if (result.empty()) return std::nullopt;
        return rowToInfo(result[0]);
    }
    catch (const std::exception& ex)
    {
        if (errorMessage) *errorMessage = std::string("读取 retool workspace 失败: ") + ex.what();
        return std::nullopt;
    }
}

std::vector<RetoolWorkspaceInfo> RetoolWorkspaceDbManager::listWorkspaces(std::string* errorMessage)
{
    std::vector<RetoolWorkspaceInfo> items;
    if (!ensureTable(errorMessage))
    {
        return items;
    }
    try
    {
        auto result = dbClient_->execSqlSync(
            "select workspace_id,email,password,mail_provider,mail_account_id,base_url,subdomain,access_token,xsrf_token,"
            "extra_cookies_json,openai_resource_uuid,openai_resource_name,anthropic_resource_uuid,anthropic_resource_name,workflow_id,workflow_api_key,agent_id,status,"
            "verify_status,last_verify_at,last_used_at,in_use_count,notes_json,created_at,updated_at from retool_workspace order by updated_at desc");
        for (const auto& row : result)
        {
            items.push_back(rowToInfo(row));
        }
    }
    catch (const std::exception& ex)
    {
        if (errorMessage) *errorMessage = std::string("列出 retool workspace 失败: ") + ex.what();
    }
    return items;
}

bool RetoolWorkspaceDbManager::updateWorkspaceStatus(const std::string& workspaceId,
                                                     const std::string& status,
                                                     const std::string& verifyStatus,
                                                     std::string* errorMessage)
{
    if (!ensureTable(errorMessage))
    {
        return false;
    }
    try
    {
        dbClient_->execSqlSync(
            "update retool_workspace set status=$1, verify_status=$2, last_verify_at=CURRENT_TIMESTAMP, updated_at=CURRENT_TIMESTAMP where workspace_id=$3",
            status,
            verifyStatus,
            workspaceId);
        return true;
    }
    catch (const std::exception& ex)
    {
        if (errorMessage) *errorMessage = std::string("更新 retool workspace 状态失败: ") + ex.what();
        return false;
    }
}

bool RetoolWorkspaceDbManager::updateWorkspaceUsage(const std::string& workspaceId,
                                                    int inUseCount,
                                                    bool touchLastUsedAt,
                                                    std::string* errorMessage)
{
    if (!ensureTable(errorMessage))
    {
        return false;
    }
    try
    {
        if (touchLastUsedAt)
        {
            dbClient_->execSqlSync(
                "update retool_workspace set in_use_count=$1, last_used_at=CURRENT_TIMESTAMP, updated_at=CURRENT_TIMESTAMP where workspace_id=$2",
                inUseCount,
                workspaceId);
        }
        else
        {
            dbClient_->execSqlSync(
                "update retool_workspace set in_use_count=$1, updated_at=CURRENT_TIMESTAMP where workspace_id=$2",
                inUseCount,
                workspaceId);
        }
        return true;
    }
    catch (const std::exception& ex)
    {
        if (errorMessage) *errorMessage = std::string("更新 retool workspace 使用状态失败: ") + ex.what();
        return false;
    }
}
