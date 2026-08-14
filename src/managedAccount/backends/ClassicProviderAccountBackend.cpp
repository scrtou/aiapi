#include <managedAccount/backends/ClassicProviderAccountBackend.h>

#include <accountManager/AccountJsonCodec.h>

namespace
{
std::string makeClassicId(const std::string& apiName, const std::string& userName)
{
    return apiName + ":" + userName;
}

bool splitClassicId(const std::string& id, std::string& apiName, std::string& userName)
{
    const auto pos = id.find(':');
    if (pos == std::string::npos) return false;
    apiName = id.substr(0, pos);
    userName = id.substr(pos + 1);
    return !apiName.empty() && !userName.empty();
}
}  // namespace

ClassicProviderAccountBackend::ClassicProviderAccountBackend(
    IAccountCatalog& accounts,
    IAccountAdminCommands& commands)
    : accounts_(&accounts), commands_(&commands)
{
}

std::vector<ManagedAccountRecord> ClassicProviderAccountBackend::list()
{
    std::vector<ManagedAccountRecord> records;
    const auto accountList = accounts_->listAccounts();
    for (const auto& [apiName, users] : accountList)
    {
        for (const auto& [userName, account] : users)
        {
            if (!account) continue;
            ManagedAccountRecord record;
            record.id = makeClassicId(apiName, userName);
            record.kind = ManagedAccountKind::ClassicProviderAccount;
            record.provider = apiName;
            record.displayName = userName;
            record.status = account->status;
            record.metadata = accountcodec::toJson(*account, true);
            records.push_back(record);
        }
    }
    return records;
}

std::optional<ManagedAccountRecord> ClassicProviderAccountBackend::get(const std::string& id)
{
    std::string apiName;
    std::string userName;
    if (!splitClassicId(id, apiName, userName))
    {
        return std::nullopt;
    }
    const auto accountList = accounts_->listAccounts();
    const auto apiIt = accountList.find(apiName);
    if (apiIt == accountList.end())
    {
        return std::nullopt;
    }
    const auto accountIt = apiIt->second.find(userName);
    if (accountIt == apiIt->second.end() || !accountIt->second)
    {
        return std::nullopt;
    }
    const auto& account = accountIt->second;
    ManagedAccountRecord record;
    record.id = id;
    record.kind = ManagedAccountKind::ClassicProviderAccount;
    record.provider = apiName;
    record.displayName = userName;
    record.status = account->status;
    record.metadata = accountcodec::toJson(*account, true);
    return record;
}

bool ClassicProviderAccountBackend::disable(const std::string& id, std::string* errorMessage)
{
    std::string apiName;
    std::string userName;
    if (!splitClassicId(id, apiName, userName))
    {
        if (errorMessage) *errorMessage = "invalid classic account id";
        return false;
    }
    if (!commands_->deleteAccountbyPost(apiName, userName))
    {
        if (errorMessage) *errorMessage = "classic account not found or failed to disable";
        return false;
    }
    return true;
}

std::optional<ManagedExecutionContext> ClassicProviderAccountBackend::buildExecutionContext(
    const std::string& id,
    std::string* errorMessage)
{
    auto record = get(id);
    if (!record)
    {
        if (errorMessage) *errorMessage = "classic account not found";
        return std::nullopt;
    }

    ManagedExecutionContext context;
    context.kind = ManagedAccountKind::ClassicProviderAccount;
    context.id = record->id;
    context.data = record->metadata;
    return context;
}
