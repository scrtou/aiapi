#include <managedAccount/backends/RetoolWorkspaceBackend.h>
#include <retoolWorkspace/RetoolWorkspaceJsonCodec.h>

RetoolWorkspaceBackend::RetoolWorkspaceBackend(
    workspace::IRetoolWorkspaceUseCase& workspaces)
    : workspaces_(&workspaces)
{
}

std::vector<ManagedAccountRecord> RetoolWorkspaceBackend::list()
{
    std::vector<ManagedAccountRecord> records;
    auto items = workspaces_->list();
    for (const auto& item : items)
    {
        ManagedAccountRecord record;
        record.id = item.workspaceId;
        record.kind = ManagedAccountKind::RetoolWorkspace;
        record.provider = "retool";
        record.displayName = item.baseUrl.empty() ? item.email : item.baseUrl;
        record.status = item.status;
        record.metadata = retoolworkspacecodec::toJson(item, true);
        records.push_back(record);
    }
    return records;
}

std::optional<ManagedAccountRecord> RetoolWorkspaceBackend::get(const std::string& id)
{
    auto workspace = workspaces_->get(id, nullptr);
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
    record.metadata = retoolworkspacecodec::toJson(*workspace, true);
    return record;
}

bool RetoolWorkspaceBackend::disable(const std::string& id, std::string* errorMessage)
{
    return workspaces_->disable(id, errorMessage);
}

std::optional<ManagedExecutionContext> RetoolWorkspaceBackend::buildExecutionContext(
    const std::string& id,
    std::string* errorMessage)
{
    auto workspace = workspaces_->get(id, errorMessage);
    if (!workspace)
    {
        return std::nullopt;
    }

    ManagedExecutionContext context;
    context.kind = ManagedAccountKind::RetoolWorkspace;
    context.id = workspace->workspaceId;
    context.data = retoolworkspacecodec::toJson(*workspace, true);
    return context;
}
