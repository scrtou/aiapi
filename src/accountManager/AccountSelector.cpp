#include <accountManager/accountManager.h>

#include <accountManager/AccountSelectionPolicy.h>
#include <accountManager/AccountWorkflowSupport.h>
#include <domain/policy/RetiredProviderPolicy.h>
#include <platform/Log.h>

#include <algorithm>
#include <vector>

namespace {
using AccountQueue = std::priority_queue<std::shared_ptr<Accountinfo_st>,
                                         std::vector<std::shared_ptr<Accountinfo_st>>,
                                         AccountCompare>;

std::shared_ptr<AccountQueue> makeQueue()
{
    return std::make_shared<AccountQueue>();
}

}  // namespace

void AccountManager::loadAccount()
{
    // A reload is one atomic replacement of both representations.  Clearing
    // only the heap used to leave a database-deleted account visible through
    // accountList, which then leaked into health and admin responses.
    {
        std::lock_guard<std::mutex> lock(accountListMutex);
        accountPoolMap.clear();
        accountList.clear();
    }

    LOG_INFO << "[账户管理] 加载账户开始";
    if (requireStore()->isTableExist()) {
        loadAccountFromDatebase();
    } else {
        loadAccountFromConfig();
    }

    {
        std::lock_guard<std::mutex> lock(accountListMutex);
        for (const auto& entry : accountPoolMap) {
            LOG_INFO << "[账户管理] API名称: " << entry.first
                     << ", 账户队列大小: " << (entry.second ? entry.second->size() : 0);
        }
    }
    LOG_INFO << "[账户管理] 加载账户完成";
}

void AccountManager::loadAccountFromConfig()
{
    LOG_INFO << "[账户管理] 从配置文件加载账户开始";
    const auto configAccountList = runtimeConfig_["account"];
    for (const auto& account : configAccountList) {
        const auto apiName = account["apiname"].empty() ? "" : account["apiname"].asString();
        const auto userName = account["username"].empty() ? "" : account["username"].asString();
        const auto passwd = account["passwd"].empty() ? "" : account["passwd"].asString();
        const auto authToken = account["authToken"].empty() ? "" : account["authToken"].asString();
        const auto useCount = account["usecount"].empty() ? 0 : account["usecount"].asInt();
        const auto tokenStatus = account["tokenStatus"].empty() ? false : account["tokenStatus"].asBool();
        const auto accountStatus = account["accountStatus"].empty() ? false : account["accountStatus"].asBool();
        const auto userTobitId = account["usertobitid"].empty() ? 0 : account["usertobitid"].asInt();
        const auto personId = account["personId"].empty() ? "" : account["personId"].asString();
        const auto createTime = account["createTime"].empty() ? "" : account["createTime"].asString();
        const auto accountType = account["accountType"].empty() ? "free" : account["accountType"].asString();
        const auto workspaceUacId = account["workspaceUacId"].empty()
            ? 0 : account["workspaceUacId"].asInt64();
        addAccount(apiName, userName, passwd, authToken, useCount, tokenStatus,
                   accountStatus, userTobitId, personId, createTime, accountType,
                   AccountStatus::ACTIVE, workspaceUacId);
    }
    LOG_INFO << "[账户管理] 从配置文件加载账户完成";
}

void AccountManager::addAccount(string apiName, string userName, string passwd,
                                string authToken, int useCount, bool tokenStatus,
                                bool accountStatus, int userTobitId, string personId,
                                string createTime, string accountType, string status,
                                std::int64_t workspaceUacId)
{
    if (retired_provider::isRetiredProviderKey(apiName)) {
        LOG_ERROR << "[账户管理] 拒绝加载已退役 Provider 账号: apiName=" << apiName;
        return;
    }

    auto account = std::make_shared<Accountinfo_st>(
        apiName, userName, passwd, authToken, useCount, tokenStatus, accountStatus,
        userTobitId, personId, createTime, accountType, status, workspaceUacId);
    std::lock_guard<std::mutex> lock(accountListMutex);
    accountList[apiName][userName] = account;

    if (account::workflow::shouldExcludeFromPool(account)) {
        LOG_WARN << "[账户管理] 账号处于非 active 生命周期状态，已从账号池排除: "
                 << userName << ", apiName=" << apiName << ", status=" << status;
        return;
    }
    if (!account::selection::isPoolMember(account)) {
        LOG_INFO << "[账户管理] 账号 " << userName << " 状态为 " << status
                 << ", 不加入账号池";
        return;
    }
    if (!accountPoolMap[apiName]) {
        accountPoolMap[apiName] = makeQueue();
    }
    accountPoolMap[apiName]->push(std::move(account));
}

bool AccountManager::addAccountbyPost(Accountinfo_st accountinfo)
{
    if (retired_provider::isRetiredProviderKey(accountinfo.apiName)) {
        LOG_ERROR << "[账户管理] 拒绝新增已退役 Provider 账号: apiName=" << accountinfo.apiName;
        return false;
    }
    auto account = std::make_shared<Accountinfo_st>(accountinfo);
    std::lock_guard<std::mutex> lock(accountListMutex);
    auto& byUser = accountList[accountinfo.apiName];
    if (byUser.find(accountinfo.userName) != byUser.end()) {
        return false;
    }
    byUser[accountinfo.userName] = account;
    if (account::selection::isPoolMember(account) &&
        !account::workflow::shouldExcludeFromPool(account)) {
        if (!accountPoolMap[accountinfo.apiName]) {
            accountPoolMap[accountinfo.apiName] = makeQueue();
        }
        accountPoolMap[accountinfo.apiName]->push(std::move(account));
    }
    return true;
}

void AccountManager::rebuildPoolLocked(const std::string& apiName)
{
    // accountListMutex must already be held.  Both keys used by AccountCompare
    // are mutable, so any mutation is followed by a full rebuild instead of a
    // stale heap repair attempt.
    const auto listIt = accountList.find(apiName);
    if (listIt == accountList.end() || listIt->second.empty()) {
        accountPoolMap.erase(apiName);
        return;
    }
    auto queue = makeQueue();
    for (const auto& [_, account] : listIt->second) {
        if (account::selection::isPoolMember(account) &&
            !account::workflow::shouldExcludeFromPool(account)) {
            queue->push(account);
        }
    }
    accountPoolMap[apiName] = std::move(queue);
}

bool AccountManager::updateAccount(Accountinfo_st accountinfo)
{
    if (retired_provider::isRetiredProviderKey(accountinfo.apiName)) {
        LOG_ERROR << "[账户管理] 拒绝更新已退役 Provider 账号: apiName=" << accountinfo.apiName;
        return false;
    }
    std::lock_guard<std::mutex> lock(accountListMutex);
    const auto apiIt = accountList.find(accountinfo.apiName);
    if (apiIt == accountList.end()) {
        return false;
    }
    const auto userIt = apiIt->second.find(accountinfo.userName);
    if (userIt == apiIt->second.end()) {
        return false;
    }
    const auto& account = userIt->second;
    account->passwd = accountinfo.passwd;
    account->authToken = accountinfo.authToken;
    account->useCount = accountinfo.useCount;
    account->tokenStatus = accountinfo.tokenStatus;
    account->accountStatus = accountinfo.accountStatus;
    account->userTobitId = accountinfo.userTobitId;
    account->personId = accountinfo.personId;
    account->accountType = accountinfo.accountType;
    account->status = accountinfo.status;
    account->workspaceUacId = accountinfo.workspaceUacId;
    rebuildPoolLocked(accountinfo.apiName);
    return true;
}

bool AccountManager::deleteAccountbyPost(string apiName, string userName)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    const auto apiIt = accountList.find(apiName);
    if (apiIt == accountList.end()) {
        return false;
    }
    const auto userIt = apiIt->second.find(userName);
    if (userIt == apiIt->second.end()) {
        return false;
    }
    userIt->second->tokenStatus = false;
    userIt->second->accountStatus = false;
    apiIt->second.erase(userIt);
    rebuildPoolLocked(apiName);
    return true;
}

void AccountManager::getAccount(string apiName, std::shared_ptr<Accountinfo_st>& account,
                                string accountType)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    const auto poolIt = accountPoolMap.find(apiName);
    if (poolIt == accountPoolMap.end() || !poolIt->second || poolIt->second->empty()) {
        LOG_ERROR << "[账户管理] 账户池 [" << apiName << "] 为空或未找到";
        return;
    }

    auto& pool = *poolIt->second;
    if (accountType.empty()) {
        account = pool.top();
        pool.pop();
        if (account && account->tokenStatus) {
            ++account->useCount;
        }
        rebuildPoolLocked(apiName);
        return;
    }

    std::vector<std::shared_ptr<Accountinfo_st>> scanned;
    while (!pool.empty()) {
        auto candidate = pool.top();
        pool.pop();
        scanned.push_back(candidate);
        if (candidate && candidate->accountType == accountType) {
            account = candidate;
            if (account->tokenStatus) {
                ++account->useCount;
            }
            break;
        }
    }
    for (const auto& candidate : scanned) {
        pool.push(candidate);
    }
    if (!account) {
        LOG_ERROR << "[账户管理] 未找到类型为 " << accountType << " 的账户, API: " << apiName;
        return;
    }
    rebuildPoolLocked(apiName);
}

bool AccountManager::getEligibleAccount(const string& apiName,
                                        std::shared_ptr<Accountinfo_st>& account,
                                        AccountRequirement requirement,
                                        const set<string>& excludedUsers)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    account.reset();
    const auto poolIt = accountPoolMap.find(apiName);
    if (poolIt == accountPoolMap.end() || !poolIt->second || poolIt->second->empty()) {
        LOG_ERROR << "[账户管理] 账户池 [" << apiName << "] 为空或未找到";
        return false;
    }

    auto& pool = *poolIt->second;
    std::vector<std::shared_ptr<Accountinfo_st>> scanned;
    std::shared_ptr<Accountinfo_st> preferred;
    std::shared_ptr<Accountinfo_st> fallback;
    while (!pool.empty()) {
        auto candidate = pool.top();
        pool.pop();
        scanned.push_back(candidate);
        if (!account::selection::matchesRequirement(candidate, apiName, requirement, excludedUsers)) {
            continue;
        }
        if (requirement != AccountRequirement::AnyValid) {
            preferred = candidate;
            break;
        }
        if (!fallback) {
            fallback = candidate;
        }
        if (candidate->accountType == "free") {
            preferred = candidate;
            break;
        }
    }
    for (const auto& candidate : scanned) {
        pool.push(candidate);
    }

    account = preferred ? preferred : fallback;
    if (!account) {
        if (excludedUsers.empty()) {
            LOG_ERROR << "[账户管理] 未找到满足要求的有效账户, API: " << apiName
                      << ", 要求: " << account::selection::requirementName(requirement);
        } else {
            LOG_WARN << "[账户管理] 未找到可切换的其它有效账户, API: " << apiName
                     << ", 要求: " << account::selection::requirementName(requirement)
                     << ", 已排除: " << excludedUsers.size();
        }
        return false;
    }
    ++account->useCount;
    rebuildPoolLocked(apiName);
    LOG_INFO << "[账户管理] 已选择有效账户: " << account->userName
             << " (" << account->accountType << "), 要求: "
             << account::selection::requirementName(requirement)
             << ", 新使用次数: " << account->useCount;
    return true;
}

void AccountManager::getAccountByUserName(const string& apiName, const string& userName,
                                          std::shared_ptr<Accountinfo_st>& account)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    const auto apiIt = accountList.find(apiName);
    if (apiIt == accountList.end()) {
        account.reset();
        LOG_ERROR << "[账户管理] 按用户名获取账户: 未找到账户 apiName=" << apiName
                  << ", userName=" << userName;
        return;
    }
    const auto userIt = apiIt->second.find(userName);
    if (userIt == apiIt->second.end()) {
        account.reset();
        LOG_ERROR << "[账户管理] 按用户名获取账户: 未找到账户 apiName=" << apiName
                  << ", userName=" << userName;
        return;
    }
    account = userIt->second;
    if (account && account->tokenStatus) {
        ++account->useCount;
        rebuildPoolLocked(apiName);
    }
}

void AccountManager::checkAccount()
{
    LOG_INFO << "[账户管理] 检查账户开始";
    LOG_INFO << "[账户管理] 检查账户完成";
}

void AccountManager::refreshAccountQueue(string apiName)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    rebuildPoolLocked(apiName);
}

void AccountManager::printAccountPoolMap()
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    LOG_INFO << "[账户管理] 打印账户池映射开始";
    for (const auto& [apiName, queue] : accountPoolMap) {
        LOG_INFO << "[账户管理] API名称: " << apiName
                 << ", 账户队列大小: " << (queue ? queue->size() : 0);
    }
    LOG_INFO << "[账户管理] 打印账户池映射完成";
}

void AccountManager::loadAccountFromDatebase()
{
    LOG_INFO << "[账户管理] 开始从数据库加载账号";
    auto rows = requireStore()->getAccountDBList();
    int retiredCount = 0;
    for (const auto& account : rows) {
        if (retired_provider::isRetiredProviderKey(account.apiName)) {
            ++retiredCount;
            LOG_ERROR << "[账户管理] 数据库仍含已退役 Provider 账号，已拒绝加载: apiName="
                      << account.apiName << "；请先执行 retire_providers_v1.sql";
            continue;
        }
        addAccount(account.apiName, account.userName, account.passwd, account.authToken,
                   account.useCount, account.tokenStatus, account.accountStatus,
                   account.userTobitId, account.personId, account.createTime,
                   account.accountType, account.status, account.workspaceUacId);
    }
    LOG_INFO << "[账户管理] 数据库加载完成，账号总数: " << rows.size()
             << "，拒绝加载已退役 Provider 账号数: " << retiredCount;
}

void AccountManager::saveAccount()
{
    saveAccountToDatebase();
}

void AccountManager::saveAccountToDatebase()
{
    const auto snapshot = getAccountList();
    for (const auto& [_, byUser] : snapshot) {
        for (const auto& [__, account] : byUser) {
            if (account) {
                requireStore()->addAccount(*account);
            }
        }
    }
}

std::map<string, std::map<string, std::shared_ptr<Accountinfo_st>>> AccountManager::getAccountList()
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    return accountList;
}

void AccountManager::setStatusAccountStatus(string apiName, string userName, bool status)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    const auto apiIt = accountList.find(apiName);
    if (apiIt == accountList.end()) return;
    const auto userIt = apiIt->second.find(userName);
    if (userIt == apiIt->second.end()) return;
    userIt->second->accountStatus = status;
    rebuildPoolLocked(apiName);
}

void AccountManager::setStatusTokenStatus(string apiName, string userName, bool status)
{
    std::shared_ptr<Accountinfo_st> pendingRefresh;
    {
        std::lock_guard<std::mutex> lock(accountListMutex);
        const auto apiIt = accountList.find(apiName);
        if (apiIt == accountList.end()) return;
        const auto userIt = apiIt->second.find(userName);
        if (userIt == apiIt->second.end()) return;
        userIt->second->tokenStatus = status;
        rebuildPoolLocked(apiName);
        if (!status && !account::workflow::shouldSkipLifecycleRefresh(userIt->second)) {
            pendingRefresh = userIt->second;
        }
    }
    if (pendingRefresh) {
        std::lock_guard<std::mutex> lock(accountListNeedUpdateMutex);
        accountListNeedUpdate.push_back(std::move(pendingRefresh));
        accountListNeedUpdateCondition.notify_one();
    }
}
