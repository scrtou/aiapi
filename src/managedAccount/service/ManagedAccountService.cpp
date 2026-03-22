#include "ManagedAccountService.h"

std::vector<ManagedAccountRecord> ManagedAccountService::listAll()
{
    auto classic = classicBackend_.list();
    auto retool = retoolBackend_.list();
    classic.insert(classic.end(), retool.begin(), retool.end());
    return classic;
}

std::vector<ManagedAccountRecord> ManagedAccountService::listByKind(ManagedAccountKind kind)
{
    switch (kind)
    {
        case ManagedAccountKind::RetoolWorkspace:
            return retoolBackend_.list();
        case ManagedAccountKind::ClassicProviderAccount:
        default:
            return classicBackend_.list();
    }
}

std::optional<ManagedAccountRecord> ManagedAccountService::get(ManagedAccountKind kind, const std::string& id)
{
    switch (kind)
    {
        case ManagedAccountKind::RetoolWorkspace:
            return retoolBackend_.get(id);
        case ManagedAccountKind::ClassicProviderAccount:
        default:
            return classicBackend_.get(id);
    }
}

bool ManagedAccountService::disable(ManagedAccountKind kind, const std::string& id, std::string* errorMessage)
{
    switch (kind)
    {
        case ManagedAccountKind::RetoolWorkspace:
            return retoolBackend_.disable(id, errorMessage);
        case ManagedAccountKind::ClassicProviderAccount:
        default:
            return classicBackend_.disable(id, errorMessage);
    }
}

std::optional<ManagedExecutionContext> ManagedAccountService::buildExecutionContext(
    ManagedAccountKind kind,
    const std::string& id,
    std::string* errorMessage)
{
    switch (kind)
    {
        case ManagedAccountKind::RetoolWorkspace:
            return retoolBackend_.buildExecutionContext(id, errorMessage);
        case ManagedAccountKind::ClassicProviderAccount:
        default:
            return classicBackend_.buildExecutionContext(id, errorMessage);
    }
}
