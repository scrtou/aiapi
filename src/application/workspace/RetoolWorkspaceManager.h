#pragma once

#include <optional>
#include <domain/model/RetoolWorkspaceInfo.h>
#include <string>
#include <vector>

#include <domain/port/IRetoolWorkspaceStore.h>
#include <memory>

class RetoolWorkspaceManager
{
  public:
    // The composition root owns this facade.  A store is deliberately a
    // construction dependency rather than a later setter: `init()` performs
    // persistence work immediately, so a mutable injection phase would make
    // the lifecycle ordering implicit again.
    explicit RetoolWorkspaceManager(std::shared_ptr<IRetoolWorkspaceStore> store);
    RetoolWorkspaceManager(const RetoolWorkspaceManager&) = delete;
    RetoolWorkspaceManager& operator=(const RetoolWorkspaceManager&) = delete;

    void init();
    bool upsertWorkspace(const RetoolWorkspaceInfo& info, std::string* errorMessage = nullptr);
    bool deleteWorkspace(const std::string& workspaceId, std::string* errorMessage = nullptr);
    std::optional<RetoolWorkspaceInfo> getWorkspace(const std::string& workspaceId,
                                                    std::string* errorMessage = nullptr);
    std::vector<RetoolWorkspaceInfo> listWorkspaces(std::string* errorMessage = nullptr);
    bool updateWorkspaceStatus(const std::string& workspaceId,
                               const std::string& status,
                               const std::string& verifyStatus,
                               std::string* errorMessage = nullptr);
    bool markWorkspaceUsageStarted(const std::string& workspaceId, std::string* errorMessage = nullptr);
    bool markWorkspaceUsageFinished(const std::string& workspaceId, std::string* errorMessage = nullptr);
    bool disableWorkspace(const std::string& workspaceId, std::string* errorMessage = nullptr);

  private:
    std::shared_ptr<IRetoolWorkspaceStore> store_;
};
