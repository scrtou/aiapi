#include <application/workspace/RetoolWorkspaceManager.h>

#include <algorithm>
#include <domain/port/IRetoolWorkspaceStore.h>
#include <platform/Log.h>

#include <utility>

namespace
{
// An unconfigured manager remains diagnosable for unit tests, but the
// fallback is owned by that one manager instance.  It is not a process-wide
// service that production code can rediscover.
class NullRetoolWorkspaceStore final : public IRetoolWorkspaceStore
{
  public:
    bool ensureTable(std::string* error) override { return fail(error); }
    bool upsertWorkspace(const RetoolWorkspaceInfo&, std::string* error) override
    {
        return fail(error);
    }
    bool deleteWorkspace(const std::string&, std::string* error) override
    {
        return fail(error);
    }
    std::optional<RetoolWorkspaceInfo> getWorkspace(const std::string&, std::string* error) override
    {
        fail(error);
        return std::nullopt;
    }
    std::vector<RetoolWorkspaceInfo> listWorkspaces(std::string* error) override
    {
        fail(error);
        return {};
    }
    bool updateWorkspaceStatus(const std::string&, const std::string&, const std::string&,
                               std::string* error) override
    {
        return fail(error);
    }
    bool updateWorkspaceUsage(const std::string&, int, bool, std::string* error) override
    {
        return fail(error);
    }

  private:
    static bool fail(std::string* error)
    {
        if (error != nullptr)
        {
            *error = "RetoolWorkspaceManager: store was not supplied to the constructor";
        }
        return false;
    }
};

std::shared_ptr<IRetoolWorkspaceStore> makeNullRetoolWorkspaceStore()
{
    return std::make_shared<NullRetoolWorkspaceStore>();
}
}  // namespace

RetoolWorkspaceManager::RetoolWorkspaceManager(std::shared_ptr<IRetoolWorkspaceStore> store)
    : store_(store ? std::move(store) : makeNullRetoolWorkspaceStore())
{
}

void RetoolWorkspaceManager::init()
{
    std::string error;
    if (!store_->ensureTable(&error))
    {
        LOG_ERROR << "[RetoolWorkspaceManager] 初始化失败: " << error;
    }
}

bool RetoolWorkspaceManager::upsertWorkspace(const RetoolWorkspaceInfo& info, std::string* errorMessage)
{
    return store_->upsertWorkspace(info, errorMessage);
}

bool RetoolWorkspaceManager::deleteWorkspace(const std::string& workspaceId, std::string* errorMessage)
{
    return store_->deleteWorkspace(workspaceId, errorMessage);
}

std::optional<RetoolWorkspaceInfo> RetoolWorkspaceManager::getWorkspace(const std::string& workspaceId,
                                                                        std::string* errorMessage)
{
    return store_->getWorkspace(workspaceId, errorMessage);
}

std::vector<RetoolWorkspaceInfo> RetoolWorkspaceManager::listWorkspaces(std::string* errorMessage)
{
    return store_->listWorkspaces(errorMessage);
}

bool RetoolWorkspaceManager::updateWorkspaceStatus(const std::string& workspaceId,
                                                   const std::string& status,
                                                   const std::string& verifyStatus,
                                                   std::string* errorMessage)
{
    return store_->updateWorkspaceStatus(
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
    return store_->updateWorkspaceUsage(
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
    return store_->updateWorkspaceUsage(
        workspaceId, nextCount, true, errorMessage);
}

bool RetoolWorkspaceManager::disableWorkspace(const std::string& workspaceId, std::string* errorMessage)
{
    return updateWorkspaceStatus(workspaceId, "disabled", "unknown", errorMessage);
}
