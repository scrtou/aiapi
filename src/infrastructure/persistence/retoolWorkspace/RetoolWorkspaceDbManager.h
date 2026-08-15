#ifndef RETOOL_WORKSPACE_DBMANAGER_H
#define RETOOL_WORKSPACE_DBMANAGER_H

#include <drogon/drogon.h>
#include <infrastructure/persistence/DbType.h>
#include <memory>
#include <optional>
#include <domain/model/RetoolWorkspaceInfo.h>
#include <domain/port/IRetoolWorkspaceStore.h>
#include <string>
#include <vector>

class RetoolWorkspaceDbManager : public IRetoolWorkspaceStore
{
  public:
    // The composition root owns this concrete adapter.  Construction is
    // intentionally side-effect free: a local test fixture must not touch
    // the process-wide Drogon app merely by creating a store object.
    RetoolWorkspaceDbManager() = default;
    RetoolWorkspaceDbManager(const RetoolWorkspaceDbManager&) = delete;
    RetoolWorkspaceDbManager& operator=(const RetoolWorkspaceDbManager&) = delete;

    /// Bind the configured DB client after Drogon configuration is available.
    /// AppWiring calls this before publishing the store to workspace services.
    void initialize();

    bool ensureTable(std::string* errorMessage = nullptr) override;
    bool upsertWorkspace(const RetoolWorkspaceInfo& info, std::string* errorMessage = nullptr) override;
    bool deleteWorkspace(const std::string& workspaceId, std::string* errorMessage = nullptr) override;
    std::optional<RetoolWorkspaceInfo> getWorkspace(const std::string& workspaceId,
                                                    std::string* errorMessage = nullptr) override;
    std::vector<RetoolWorkspaceInfo> listWorkspaces(std::string* errorMessage = nullptr) override;
    bool updateWorkspaceStatus(const std::string& workspaceId,
                               const std::string& status,
                               const std::string& verifyStatus,
                               std::string* errorMessage = nullptr) override;
    bool updateWorkspaceUsage(const std::string& workspaceId,
                              int inUseCount,
                              bool touchLastUsedAt,
                              std::string* errorMessage = nullptr) override;

  private:
    bool hasDbClient(std::string* errorMessage) const;
    void detectDbType();
    std::string createTableSql() const;
    bool ensureColumns(std::string* errorMessage = nullptr);

    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    DbType dbType_ = DbType::PostgreSQL;
    bool initialized_ = false;
};

#endif
