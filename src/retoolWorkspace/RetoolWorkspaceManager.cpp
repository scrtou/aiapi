#include "RetoolWorkspaceManager.h"

#include <algorithm>
#include <dbManager/retoolWorkspace/RetoolWorkspaceDbManager.h>

void RetoolWorkspaceManager::init()
{
    std::string error;
    if (!RetoolWorkspaceDbManager::getInstance()->ensureTable(&error))
    {
        LOG_ERROR << "[RetoolWorkspaceManager] 初始化失败: " << error;
    }
}

bool RetoolWorkspaceManager::upsertWorkspace(const RetoolWorkspaceInfo& info, std::string* errorMessage)
{
    return RetoolWorkspaceDbManager::getInstance()->upsertWorkspace(info, errorMessage);
}

bool RetoolWorkspaceManager::deleteWorkspace(const std::string& workspaceId, std::string* errorMessage)
{
    return RetoolWorkspaceDbManager::getInstance()->deleteWorkspace(workspaceId, errorMessage);
}

std::optional<RetoolWorkspaceInfo> RetoolWorkspaceManager::getWorkspace(const std::string& workspaceId,
                                                                        std::string* errorMessage)
{
    return RetoolWorkspaceDbManager::getInstance()->getWorkspace(workspaceId, errorMessage);
}

std::vector<RetoolWorkspaceInfo> RetoolWorkspaceManager::listWorkspaces(std::string* errorMessage)
{
    return RetoolWorkspaceDbManager::getInstance()->listWorkspaces(errorMessage);
}

bool RetoolWorkspaceManager::updateWorkspaceStatus(const std::string& workspaceId,
                                                   const std::string& status,
                                                   const std::string& verifyStatus,
                                                   std::string* errorMessage)
{
    return RetoolWorkspaceDbManager::getInstance()->updateWorkspaceStatus(
        workspaceId, status, verifyStatus, errorMessage);
}

bool RetoolWorkspaceManager::markWorkspaceUsageStarted(const std::string& workspaceId, std::string* errorMessage)
{
    auto workspace = getWorkspace(workspaceId, errorMessage);
    if (!workspace)
    {
        return false;
    }
    const int nextCount = std::max(0, workspace->inUseCount) + 1;
    return RetoolWorkspaceDbManager::getInstance()->updateWorkspaceUsage(
        workspaceId, nextCount, true, errorMessage);
}

bool RetoolWorkspaceManager::markWorkspaceUsageFinished(const std::string& workspaceId, std::string* errorMessage)
{
    auto workspace = getWorkspace(workspaceId, errorMessage);
    if (!workspace)
    {
        return false;
    }
    const int nextCount = std::max(0, workspace->inUseCount - 1);
    return RetoolWorkspaceDbManager::getInstance()->updateWorkspaceUsage(
        workspaceId, nextCount, true, errorMessage);
}

bool RetoolWorkspaceManager::disableWorkspace(const std::string& workspaceId, std::string* errorMessage)
{
    return updateWorkspaceStatus(workspaceId, "disabled", "unknown", errorMessage);
}
