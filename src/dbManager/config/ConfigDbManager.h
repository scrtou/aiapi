#ifndef CONFIG_DBMANAGER_H
#define CONFIG_DBMANAGER_H

#include <drogon/drogon.h>
#include <memory>
#include <optional>
#include <string>
#include <map>
#include <dbManager/DbType.h>

class ConfigDbManager {
  public:
    static std::shared_ptr<ConfigDbManager> getInstance()
    {
        static std::shared_ptr<ConfigDbManager> instance;
        if (instance == nullptr) {
            instance = std::make_shared<ConfigDbManager>();
            instance->dbClient_ = drogon::app().getDbClient("aichatpg");
            instance->detectDbType();
        }
        return instance;
    }

    bool ensureTable(std::string* errorMessage = nullptr);
    std::optional<std::string> getValue(const std::string& key, std::string* errorMessage = nullptr);
    bool setValue(const std::string& key, const std::string& value, std::string* errorMessage = nullptr);
    bool setValues(const std::map<std::string, std::string>& entries, std::string* errorMessage = nullptr);
    DbType getDbType() const { return dbType_; }

  private:
    void detectDbType();
    std::string getCreateTableSql() const;

    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    DbType dbType_ = DbType::PostgreSQL;
};

#endif
