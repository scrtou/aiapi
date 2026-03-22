#include "ClassicProviderAccountBackend.h"

#include <accountManager/accountManager.h>

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

std::vector<ManagedAccountRecord> ClassicProviderAccountBackend::list()
{
    std::vector<ManagedAccountRecord> records;
    const auto accountList = AccountManager::getInstance().getAccountList();
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
            record.metadata = account->toJson();
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
    std::shared_ptr<Accountinfo_st> account;
    AccountManager::getInstance().getAccountByUserName(apiName, userName, account);
    if (!account)
    {
        return std::nullopt;
    }
    ManagedAccountRecord record;
    record.id = id;
    record.kind = ManagedAccountKind::ClassicProviderAccount;
    record.provider = apiName;
    record.displayName = userName;
    record.status = account->status;
    record.metadata = account->toJson();
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
    if (!AccountManager::getInstance().deleteAccountbyPost(apiName, userName))
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
