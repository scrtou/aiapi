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
    static RetoolWorkspaceManager& getInstance()
    {
        static RetoolWorkspaceManager instance;
        return instance;
    }

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

    // R4 依赖倒置：由组合根(main.cc)注入具体实现。
    void setStore(std::shared_ptr<IRetoolWorkspaceStore> store);

  private:
    RetoolWorkspaceManager() = default;
    IRetoolWorkspaceStore* requireStore();

    std::shared_ptr<IRetoolWorkspaceStore> store_;
};
