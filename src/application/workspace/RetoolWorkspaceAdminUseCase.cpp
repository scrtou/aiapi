#include <application/workspace/RetoolWorkspaceAdminUseCase.h>

namespace workspace {

RetoolWorkspaceAdminUseCase::RetoolWorkspaceAdminUseCase(
    IRetoolWorkspaceUseCase& workspaces, IRetoolWorkspaceProvisioner& provisioner)
    : workspaces_(workspaces), provisioner_(provisioner)
{
}

RetoolWorkspaceInfo RetoolWorkspaceAdminUseCase::provision(
    const std::string& requestJson)
{
    return provisioner_.provision(requestJson);
}

bool RetoolWorkspaceAdminUseCase::upsert(RetoolWorkspaceInfo& info, std::string* error)
{
    return workspaces_.upsert(info, error);
}

std::optional<RetoolWorkspaceInfo> RetoolWorkspaceAdminUseCase::get(
    const std::string& workspaceId, std::string* error)
{
    return workspaces_.get(workspaceId, error);
}

bool RetoolWorkspaceAdminUseCase::hasExecutionContext(
    const RetoolWorkspaceInfo& workspace) const
{
    return workspaces_.hasExecutionContext(workspace);
}

std::vector<RetoolWorkspaceInfo> RetoolWorkspaceAdminUseCase::list(std::string* error)
{
    return workspaces_.list(error);
}

bool RetoolWorkspaceAdminUseCase::markUsageStarted(
    const std::string& workspaceId, std::string* error)
{
    return workspaces_.markUsageStarted(workspaceId, error);
}

bool RetoolWorkspaceAdminUseCase::markUsageFinished(
    const std::string& workspaceId, std::string* error)
{
    return workspaces_.markUsageFinished(workspaceId, error);
}

bool RetoolWorkspaceAdminUseCase::disable(const std::string& workspaceId,
                                          std::string* error)
{
    return workspaces_.disable(workspaceId, error);
}

bool RetoolWorkspaceAdminUseCase::enable(const std::string& workspaceId,
                                         std::string* nextStatus,
                                         std::string* verifyStatus,
                                         std::string* error)
{
    return workspaces_.enable(workspaceId, nextStatus, verifyStatus, error);
}

bool RetoolWorkspaceAdminUseCase::remove(const std::string& workspaceId,
                                         std::string* error)
{
    return workspaces_.remove(workspaceId, error);
}

bool RetoolWorkspaceAdminUseCase::verify(const std::string& workspaceId,
                                         bool* ready,
                                         std::string* verifyStatus,
                                         RetoolWorkspaceInfo* workspace,
                                         std::string* error)
{
    return workspaces_.verify(workspaceId, ready, verifyStatus, workspace, error);
}

PoolStatus RetoolWorkspaceAdminUseCase::poolStatus()
{
    return workspaces_.poolStatus();
}

}  // namespace workspace
