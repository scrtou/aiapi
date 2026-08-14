#pragma once

#include <domain/port/IRetoolWorkspaceAdminUseCase.h>

namespace workspace {

class RetoolWorkspaceAdminUseCase final : public IRetoolWorkspaceAdminUseCase
{
  public:
    RetoolWorkspaceAdminUseCase(IRetoolWorkspaceUseCase& workspaces,
                                IRetoolWorkspaceProvisioner& provisioner);

    RetoolWorkspaceInfo provision(const std::string& requestJson) override;
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
    IRetoolWorkspaceUseCase& workspaces_;
    IRetoolWorkspaceProvisioner& provisioner_;
};

}  // namespace workspace
