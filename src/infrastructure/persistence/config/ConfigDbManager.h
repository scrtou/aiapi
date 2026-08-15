#ifndef CONFIG_DBMANAGER_H
#define CONFIG_DBMANAGER_H

#include <drogon/drogon.h>
#include <memory>
#include <optional>
#include <string>
#include <map>
#include <infrastructure/persistence/DbType.h>
#include <domain/port/IKeyValueConfigStore.h>

class ConfigDbManager : public IKeyValueConfigStore {
  public:
    // The composition root owns this adapter.  Creating a local fixture is
    // deliberately side-effect free; only AppWiring may bind Drogon's client.
    ConfigDbManager() = default;
    ConfigDbManager(const ConfigDbManager&) = delete;
    ConfigDbManager& operator=(const ConfigDbManager&) = delete;

    /// Bind the configured database client after the Drogon app is configured.
    void initialize();

    bool ensureTable(std::string* errorMessage = nullptr) override;
    std::optional<std::string> getValue(const std::string& key, std::string* errorMessage = nullptr) override;
    bool setValue(const std::string& key, const std::string& value, std::string* errorMessage = nullptr);
    bool setValues(const std::map<std::string, std::string>& entries, std::string* errorMessage = nullptr) override;
    DbType getDbType() const { return dbType_; }

  private:
    bool hasDbClient(std::string* errorMessage) const;
    void detectDbType();
    std::string getCreateTableSql() const;

    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    DbType dbType_ = DbType::PostgreSQL;
    bool initialized_ = false;
};

#endif
