#include <retoolWorkspace/RetoolWorkspaceManager.h>

#include <algorithm>
#include <domain/port/IRetoolWorkspaceStore.h>
#include <trantor/utils/Logger.h>

void RetoolWorkspaceManager::init()
{
    std::string error;
    if (!requireStore()->ensureTable(&error))
    {
        LOG_ERROR << "[RetoolWorkspaceManager] 初始化失败: " << error;
    }
}

bool RetoolWorkspaceManager::upsertWorkspace(const RetoolWorkspaceInfo& info, std::string* errorMessage)
{
    return requireStore()->upsertWorkspace(info, errorMessage);
}

bool RetoolWorkspaceManager::deleteWorkspace(const std::string& workspaceId, std::string* errorMessage)
{
    return requireStore()->deleteWorkspace(workspaceId, errorMessage);
}

std::optional<RetoolWorkspaceInfo> RetoolWorkspaceManager::getWorkspace(const std::string& workspaceId,
                                                                        std::string* errorMessage)
{
    return requireStore()->getWorkspace(workspaceId, errorMessage);
}

std::vector<RetoolWorkspaceInfo> RetoolWorkspaceManager::listWorkspaces(std::string* errorMessage)
{
    return requireStore()->listWorkspaces(errorMessage);
}

bool RetoolWorkspaceManager::updateWorkspaceStatus(const std::string& workspaceId,
                                                   const std::string& status,
                                                   const std::string& verifyStatus,
                                                   std::string* errorMessage)
{
    return requireStore()->updateWorkspaceStatus(
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
    return requireStore()->updateWorkspaceUsage(
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
    return requireStore()->updateWorkspaceUsage(
        workspaceId, nextCount, true, errorMessage);
}

bool RetoolWorkspaceManager::disableWorkspace(const std::string& workspaceId, std::string* errorMessage)
{
    return updateWorkspaceStatus(workspaceId, "disabled", "unknown", errorMessage);
}

// ---------------------------------------------------------------------------
// R4 依赖倒置支撑代码
// ---------------------------------------------------------------------------
namespace
{
// 未注入实现时的 Null Object：不崩溃，返回失败并给出可诊断的错误信息。
class NullRetoolWorkspaceStore : public IRetoolWorkspaceStore
{
  public:
    bool ensureTable(std::string* e) override { return fail(e); }
    bool upsertWorkspace(const RetoolWorkspaceInfo&, std::string* e) override { return fail(e); }
    bool deleteWorkspace(const std::string&, std::string* e) override { return fail(e); }
    std::optional<RetoolWorkspaceInfo> getWorkspace(const std::string&, std::string* e) override
    {
        fail(e);
        return std::nullopt;
    }
    std::vector<RetoolWorkspaceInfo> listWorkspaces(std::string* e) override
    {
        fail(e);
        return {};
    }
    bool updateWorkspaceStatus(const std::string&, const std::string&, const std::string&, std::string* e) override
    {
        return fail(e);
    }
    bool updateWorkspaceUsage(const std::string&, int, bool, std::string* e) override { return fail(e); }

  private:
    static bool fail(std::string* e)
    {
        if (e != nullptr)
        {
            *e = "RetoolWorkspaceManager: store 未注入（应由 main.cc 调 setStore）";
        }
        return false;
    }
};
}  // namespace

void RetoolWorkspaceManager::setStore(std::shared_ptr<IRetoolWorkspaceStore> store)
{
    store_ = std::move(store);
}

IRetoolWorkspaceStore* RetoolWorkspaceManager::requireStore()
{
    if (store_ != nullptr)
    {
        return store_.get();
    }
    static NullRetoolWorkspaceStore nullStore;
    return &nullStore;
}
