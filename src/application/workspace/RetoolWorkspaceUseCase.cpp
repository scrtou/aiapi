#include <application/workspace/RetoolWorkspaceUseCase.h>

#include <algorithm>

namespace workspace {
namespace {

void mergePreservingExisting(RetoolWorkspaceInfo& incoming,
                             const RetoolWorkspaceInfo& existing)
{
    auto keep = [](std::string& target, const std::string& fallback) {
        if (target.empty()) target = fallback;
    };
    keep(incoming.email, existing.email);
    keep(incoming.password, existing.password);
    keep(incoming.mailProvider, existing.mailProvider);
    keep(incoming.mailAccountId, existing.mailAccountId);
    keep(incoming.baseUrl, existing.baseUrl);
    keep(incoming.subdomain, existing.subdomain);
    keep(incoming.accessToken, existing.accessToken);
    keep(incoming.xsrfToken, existing.xsrfToken);
    keep(incoming.extraCookiesJson, existing.extraCookiesJson);
    keep(incoming.openaiResourceUuid, existing.openaiResourceUuid);
    keep(incoming.openaiResourceName, existing.openaiResourceName);
    keep(incoming.anthropicResourceUuid, existing.anthropicResourceUuid);
    keep(incoming.anthropicResourceName, existing.anthropicResourceName);
    keep(incoming.workflowId, existing.workflowId);
    keep(incoming.workflowApiKey, existing.workflowApiKey);
    keep(incoming.agentId, existing.agentId);
    keep(incoming.status, existing.status);
    keep(incoming.verifyStatus, existing.verifyStatus);
    keep(incoming.lastVerifyAt, existing.lastVerifyAt);
    keep(incoming.lastUsedAt, existing.lastUsedAt);
    keep(incoming.notesJson, existing.notesJson);
    keep(incoming.createdAt, existing.createdAt);
    keep(incoming.updatedAt, existing.updatedAt);
    if (incoming.inUseCount == 0 && existing.inUseCount != 0)
        incoming.inUseCount = existing.inUseCount;
}

std::string configValue(IKeyValueConfigStore* config, const std::string& key)
{
    if (!config) return {};
    return config->getValue(key, nullptr).value_or("");
}

}  // namespace

RetoolWorkspaceUseCase::RetoolWorkspaceUseCase(
    IRetoolWorkspaceStore* workspaces, IKeyValueConfigStore* config,
    IChannelCatalog* channels)
    : workspaces_(workspaces), config_(config), channels_(channels) {}

bool RetoolWorkspaceUseCase::upsert(RetoolWorkspaceInfo& info, std::string* error)
{
    if (!workspaces_) { if (error) *error = "workspace store unavailable"; return false; }
    if (auto existing = workspaces_->getWorkspace(info.workspaceId, error))
        mergePreservingExisting(info, *existing);
    else if (error) error->clear();
    if (info.baseUrl.empty()) {
        if (error) *error = "baseUrl is required";
        return false;
    }
    return workspaces_->upsertWorkspace(info, error);
}

std::optional<RetoolWorkspaceInfo> RetoolWorkspaceUseCase::get(
    const std::string& id, std::string* error)
{
    if (!workspaces_) { if (error) *error = "workspace store unavailable"; return std::nullopt; }
    return workspaces_->getWorkspace(id, error);
}

bool RetoolWorkspaceUseCase::hasExecutionContext(
    const RetoolWorkspaceInfo& workspace) const
{
    return !workspace.workspaceId.empty();
}

std::vector<RetoolWorkspaceInfo> RetoolWorkspaceUseCase::list(std::string* error)
{
    if (!workspaces_) { if (error) *error = "workspace store unavailable"; return {}; }
    return workspaces_->listWorkspaces(error);
}

bool RetoolWorkspaceUseCase::markUsageStarted(const std::string& id,
                                              std::string* error)
{
    auto current = get(id, error);
    if (!current) return false;
    return workspaces_->updateWorkspaceUsage(
        id, std::max(0, current->inUseCount) + 1, true, error);
}

bool RetoolWorkspaceUseCase::markUsageFinished(const std::string& id,
                                               std::string* error)
{
    auto current = get(id, error);
    if (!current) return false;
    return workspaces_->updateWorkspaceUsage(
        id, std::max(0, current->inUseCount - 1), true, error);
}

bool RetoolWorkspaceUseCase::disable(const std::string& id, std::string* error)
{
    if (!workspaces_) { if (error) *error = "workspace store unavailable"; return false; }
    return workspaces_->updateWorkspaceStatus(id, "disabled", "unknown", error);
}

bool RetoolWorkspaceUseCase::enable(const std::string& id, std::string* next,
                                    std::string* verifyStatus, std::string* error)
{
    auto current = get(id, error);
    if (!current) return false;
    *verifyStatus = current->verifyStatus.empty() ? "unknown" : current->verifyStatus;
    *next = (*verifyStatus == "ready" || *verifyStatus == "passed")
                ? "ready" : "needs_attention";
    return workspaces_->updateWorkspaceStatus(id, *next, *verifyStatus, error);
}

bool RetoolWorkspaceUseCase::remove(const std::string& id, std::string* error)
{
    if (!workspaces_) { if (error) *error = "workspace store unavailable"; return false; }
    return workspaces_->deleteWorkspace(id, error);
}

bool RetoolWorkspaceUseCase::verify(const std::string& id, bool* ready,
                                    std::string* verifyStatus,
                                    RetoolWorkspaceInfo* result,
                                    std::string* error)
{
    auto current = get(id, error);
    if (!current) return false;
    *ready = !current->baseUrl.empty() && !current->accessToken.empty() &&
             !current->xsrfToken.empty() && !current->workflowId.empty() &&
             !current->agentId.empty();
    *verifyStatus = *ready ? "ready" : "incomplete";
    if (!workspaces_->updateWorkspaceStatus(
            id, *ready ? current->status : "needs_attention", *verifyStatus, error))
        return false;
    *result = *current;
    return true;
}

PoolStatus RetoolWorkspaceUseCase::poolStatus()
{
    PoolStatus result;
    const auto workspaces = list();
    result.total = workspaces.size();
    for (const auto& item : workspaces) {
        if (!item.lastUsedAt.empty() &&
            (result.latestUsedAt.empty() || item.lastUsedAt > result.latestUsedAt))
            result.latestUsedAt = item.lastUsedAt;
        if (item.status == "disabled") ++result.disabled;
        else if (item.inUseCount > 0) ++result.inUse;
        else ++result.idle;
    }
    if (channels_) for (const auto& channel : channels_->listChannels())
        if (channel.channelName == "retoolapi") { result.channel = channel; break; }
    const auto failures = configValue(config_, "retoolapi.provision.consecutive_failures");
    try { if (!failures.empty()) result.consecutiveFailures = std::stoi(failures); } catch (...) {}
    result.lastFailureAt = configValue(config_, "retoolapi.provision.last_failure_at");
    result.lastFailureReason = configValue(config_, "retoolapi.provision.last_failure_reason");
    result.cooldownUntil = configValue(config_, "retoolapi.provision.cooldown_until");
    return result;
}

}  // namespace workspace
