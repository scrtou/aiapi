#pragma once

#include <domain/port/IChannelCatalog.h>
#include <domain/model/RetoolWorkspaceInfo.h>
#include <domain/port/IKeyValueConfigStore.h>
#include <domain/port/IRetoolWorkspaceStore.h>
#include <domain/port/IRetoolWorkspaceUseCase.h>

#include <optional>
#include <string>
#include <vector>

namespace workspace {

class RetoolWorkspaceUseCase final : public IRetoolWorkspaceUseCase
{
  public:
    RetoolWorkspaceUseCase(IRetoolWorkspaceStore* workspaces,
                           IKeyValueConfigStore* config,
                           IChannelCatalog* channels = nullptr);

    void setChannels(IChannelCatalog* channels) { channels_ = channels; }
    bool upsert(RetoolWorkspaceInfo& info, std::string* error) override;
    std::optional<RetoolWorkspaceInfo> get(
        const std::string& workspaceId, std::string* error) override;
    bool hasExecutionContext(const RetoolWorkspaceInfo& workspace) const override;
    std::vector<RetoolWorkspaceInfo> list(std::string* error = nullptr) override;
    bool markUsageStarted(const std::string& workspaceId,
                          std::string* error) override;
    bool markUsageFinished(const std::string& workspaceId,
                           std::string* error) override;
    bool disable(const std::string& workspaceId, std::string* error) override;
    bool enable(const std::string& workspaceId, std::string* nextStatus,
                std::string* verifyStatus, std::string* error) override;
    bool remove(const std::string& workspaceId, std::string* error) override;
    bool verify(const std::string& workspaceId, bool* ready,
                std::string* verifyStatus, RetoolWorkspaceInfo* workspace,
                std::string* error) override;
    PoolStatus poolStatus() override;

  private:
    IRetoolWorkspaceStore* workspaces_;
    IKeyValueConfigStore* config_;
    IChannelCatalog* channels_;
};

}  // namespace workspace
