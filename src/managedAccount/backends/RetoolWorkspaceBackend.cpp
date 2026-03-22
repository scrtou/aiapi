#include "RetoolWorkspaceBackend.h"

#include <retoolWorkspace/RetoolWorkspaceManager.h>

std::vector<ManagedAccountRecord> RetoolWorkspaceBackend::list()
{
    std::vector<ManagedAccountRecord> records;
    auto items = RetoolWorkspaceManager::getInstance().listWorkspaces();
    for (const auto& item : items)
    {
        ManagedAccountRecord record;
        record.id = item.workspaceId;
        record.kind = ManagedAccountKind::RetoolWorkspace;
        record.provider = "retool";
        record.displayName = item.baseUrl.empty() ? item.email : item.baseUrl;
        record.status = item.status;
        record.metadata = item.toJson(false);
        records.push_back(record);
    }
    return records;
}

std::optional<ManagedAccountRecord> RetoolWorkspaceBackend::get(const std::string& id)
{
    auto workspace = RetoolWorkspaceManager::getInstance().getWorkspace(id);
    if (!workspace)
    {
        return std::nullopt;
    }

    ManagedAccountRecord record;
    record.id = workspace->workspaceId;
    record.kind = ManagedAccountKind::RetoolWorkspace;
    record.provider = "retool";
    record.displayName = workspace->baseUrl.empty() ? workspace->email : workspace->baseUrl;
    record.status = workspace->status;
    record.metadata = workspace->toJson(false);
    return record;
}

bool RetoolWorkspaceBackend::disable(const std::string& id, std::string* errorMessage)
{
    return RetoolWorkspaceManager::getInstance().disableWorkspace(id, errorMessage);
}

std::optional<ManagedExecutionContext> RetoolWorkspaceBackend::buildExecutionContext(
    const std::string& id,
    std::string* errorMessage)
{
    auto workspace = RetoolWorkspaceManager::getInstance().getWorkspace(id, errorMessage);
    if (!workspace)
    {
        return std::nullopt;
    }

    ManagedExecutionContext context;
    context.kind = ManagedAccountKind::RetoolWorkspace;
    context.id = workspace->workspaceId;
    context.data = workspace->toJson(true);
    return context;
}
