#ifndef RETOOL_WORKSPACE_DBMANAGER_H
#define RETOOL_WORKSPACE_DBMANAGER_H

#include <drogon/drogon.h>
#include <dbManager/DbType.h>
#include <memory>
#include <optional>
#include <retoolWorkspace/RetoolWorkspaceInfo.h>
#include <string>
#include <vector>

class RetoolWorkspaceDbManager
{
  public:
    static std::shared_ptr<RetoolWorkspaceDbManager> getInstance()
    {
        static std::shared_ptr<RetoolWorkspaceDbManager> instance;
        if (instance == nullptr)
        {
            instance = std::make_shared<RetoolWorkspaceDbManager>();
            instance->dbClient_ = drogon::app().getDbClient("aichatpg");
            instance->detectDbType();
        }
        return instance;
    }

    bool ensureTable(std::string* errorMessage = nullptr);
    bool upsertWorkspace(const RetoolWorkspaceInfo& info, std::string* errorMessage = nullptr);
    bool deleteWorkspace(const std::string& workspaceId, std::string* errorMessage = nullptr);
    std::optional<RetoolWorkspaceInfo> getWorkspace(const std::string& workspaceId,
                                                    std::string* errorMessage = nullptr);
    std::vector<RetoolWorkspaceInfo> listWorkspaces(std::string* errorMessage = nullptr);
    bool updateWorkspaceStatus(const std::string& workspaceId,
                               const std::string& status,
                               const std::string& verifyStatus,
                               std::string* errorMessage = nullptr);
    bool updateWorkspaceUsage(const std::string& workspaceId,
                              int inUseCount,
                              bool touchLastUsedAt,
                              std::string* errorMessage = nullptr);

  private:
    void detectDbType();
    std::string createTableSql() const;
    bool ensureColumns(std::string* errorMessage = nullptr);

    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    DbType dbType_ = DbType::PostgreSQL;
};

#endif
