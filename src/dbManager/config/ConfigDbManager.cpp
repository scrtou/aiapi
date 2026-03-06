#include "ConfigDbManager.h"
#include <algorithm>

void ConfigDbManager::detectDbType()
{
    auto customConfig = drogon::app().getCustomConfig();
    std::string dbTypeStr = "postgresql";

    if (customConfig.isMember("dbtype")) {
        dbTypeStr = customConfig["dbtype"].asString();
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

std::string ConfigDbManager::getCreateTableSql() const
{
    switch (dbType_) {
        case DbType::SQLite3:
            return R"(
                CREATE TABLE IF NOT EXISTS app_config (
                    config_key TEXT PRIMARY KEY,
                    config_value TEXT NOT NULL,
                    updatetime DATETIME DEFAULT CURRENT_TIMESTAMP
                );
            )";
        case DbType::MySQL:
            return R"(
                CREATE TABLE IF NOT EXISTS app_config (
                    config_key VARCHAR(255) PRIMARY KEY,
                    config_value TEXT NOT NULL,
                    updatetime DATETIME DEFAULT CURRENT_TIMESTAMP
                ) ENGINE=InnoDB;
            )";
        case DbType::PostgreSQL:
        default:
            return R"(
                CREATE TABLE IF NOT EXISTS app_config (
                    config_key VARCHAR(255) PRIMARY KEY,
                    config_value TEXT NOT NULL,
                    updatetime TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                );
            )";
    }
}

bool ConfigDbManager::ensureTable(std::string* errorMessage)
{
    if (!dbClient_) {
        if (errorMessage) {
            *errorMessage = "未获取到数据库客户端";
        }
        return false;
    }

    try {
        dbClient_->execSqlSync(getCreateTableSql());
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage) {
            *errorMessage = std::string("创建配置表失败: ") + ex.what();
        }
        return false;
    }
}

std::optional<std::string> ConfigDbManager::getValue(const std::string& key, std::string* errorMessage)
{
    if (!dbClient_) {
        if (errorMessage) {
            *errorMessage = "未获取到数据库客户端";
        }
        return std::nullopt;
    }

    try {
        auto result = dbClient_->execSqlSync(
            "select config_value from app_config where config_key=$1 limit 1",
            key);
        if (result.empty()) {
            return std::nullopt;
        }
        return result[0]["config_value"].as<std::string>();
    } catch (const std::exception& ex) {
        if (errorMessage) {
            *errorMessage = std::string("读取配置项失败: ") + ex.what();
        }
        return std::nullopt;
    }
}

bool ConfigDbManager::setValue(const std::string& key, const std::string& value, std::string* errorMessage)
{
    if (!dbClient_) {
        if (errorMessage) {
            *errorMessage = "未获取到数据库客户端";
        }
        return false;
    }

    try {
        auto existing = dbClient_->execSqlSync(
            "select config_key from app_config where config_key=$1 limit 1",
            key);
        if (existing.empty()) {
            dbClient_->execSqlSync(
                "insert into app_config(config_key, config_value, updatetime) values($1, $2, CURRENT_TIMESTAMP)",
                key,
                value);
        } else {
            dbClient_->execSqlSync(
                "update app_config set config_value=$1, updatetime=CURRENT_TIMESTAMP where config_key=$2",
                value,
                key);
        }
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage) {
            *errorMessage = std::string("写入配置项失败: ") + ex.what();
        }
        return false;
    }
}

bool ConfigDbManager::setValues(const std::map<std::string, std::string>& entries, std::string* errorMessage)
{
    for (const auto& [key, value] : entries) {
        if (!setValue(key, value, errorMessage)) {
            return false;
        }
    }
    return true;
}
