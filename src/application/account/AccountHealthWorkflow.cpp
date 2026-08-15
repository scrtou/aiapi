#include <application/account/accountManager.h>

#include <application/account/AccountWorkflowSupport.h>
#include <application/account/RetoolProvisionHealth.h>
#include <application/account/LoginResponseLogSummary.h>
#include <platform/Base64.h>
#include <platform/LocalDateTime.h>
#include <platform/Log.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <vector>

void AccountManager::checkChannelAccountCounts()
{
    if (!getAccountAutomationSettings().autoRegisterEnabled) {
        LOG_INFO << "[账户管理] 自动补注册已禁用，跳过渠道账号数量检查";
        return;
    }
    for (const auto& channel : requireChannelStore()->getChannelList()) {
        checkChannelAccountCount(channel.channelName);
    }
}

void AccountManager::checkChannelAccountCount(string apiName)
{
    const auto channels = requireChannelStore()->getChannelList();
    const auto channelIt = std::find_if(channels.begin(), channels.end(),
        [&apiName](const Channelinfo_st& channel) { return channel.channelName == apiName; });
    if (channelIt == channels.end()) {
        LOG_WARN << "[账户管理] 未找到渠道，跳过账号数量检查: " << apiName;
        return;
    }
    const auto& channel = *channelIt;
    if (!channel.channelStatus || channel.accountCount <= 0) {
        return;
    }

    int currentCount = 0;
    if (channel.channelName == "retoolapi") {
        if (!retoolProvisionClock_) {
            LOG_ERROR << "[账户管理] Retool provision clock 未注入，跳过自动补注册";
            return;
        }
        const auto health = retoolProvision::loadRetoolProvisionHealth(*configStore_, nullptr);
        if (retoolProvision::isRetoolProvisionCoolingDown(health, *retoolProvisionClock_)) {
            LOG_WARN << "[账户管理] Retool 渠道处于冷却期，跳过自动补注册";
            return;
        }
        for (const auto& workspace : workspaceUseCase_
                 ? workspaceUseCase_->list() : std::vector<RetoolWorkspaceInfo>{}) {
            if (account::workflow::isRetoolWorkspaceActive(workspace)) ++currentCount;
        }
    } else {
        currentCount = requireStore()->countAccountsByChannel(channel.channelName, true);
    }

    const int needed = channel.accountCount - currentCount;
    if (needed <= 0) return;
    for (int index = 0; index < needed; ++index) {
        if (!autoRegisterAccount(channel.channelName)) break;
        if (index + 1 < needed) {
            clock_->sleepFor(std::chrono::seconds(5));
        }
    }
}

bool AccountManager::deleteUpstreamAccount(const Accountinfo_st& account)
{
    if (account.userName.empty() || account.passwd.empty() || account.authToken.empty()) {
        LOG_ERROR << "[上游删除] 账号缺少删除所需凭据";
        return false;
    }
    const std::string credentials = account.userName + ":" + account.passwd;
    const std::string basic = platform::base64Encode(credentials);
    try {
        account::HttpRequest confirmationRequest;
        confirmationRequest.method = account::HttpMethod::Post;
        confirmationRequest.path = "/v2/token";
        confirmationRequest.headers["Authorization"] = "Basic " + basic;
        confirmationRequest.headers["Content-Type"] = "application/json; charset=utf-8";
        Json::Value confirmationBody;
        confirmationBody["tokenType"] = 12;
        confirmationBody["isConfirmation"] = false;
        confirmationBody["locationId"] = 234191;
        confirmationBody["deviceId"] = "1a0e1c3b-bc2e-4dd8-863a-e7061e35ccff";
        confirmationBody["createIfNotExists"] = false;
        confirmationRequest.body = confirmationBody.toStyledString();
        const auto [confirmationResult, confirmationResponse] = sendHttpRequest(
            "https://auth.tobit.com", confirmationRequest, 30.0);
        if (confirmationResult != account::HttpResultCode::Ok || !confirmationResponse ||
            confirmationResponse->statusCode != 200) {
            return false;
        }

        Json::Value confirmation;
        std::string errors;
        if (!account::workflow::parseJsonBody(confirmationResponse->body,
                                               confirmation, errors)) {
            return false;
        }
        const std::string confirmationToken = confirmation.get("token", "").asString();
        if (confirmationToken.empty()) return false;

        account::HttpRequest deleteRequest;
        deleteRequest.method = account::HttpMethod::Delete;
        deleteRequest.path = "/AccountService/v1.0/chayns/User";
        deleteRequest.headers["Authorization"] = "Bearer " + account.authToken;
        deleteRequest.headers["x-confirmation-token"] = "bearer " + confirmationToken;
        deleteRequest.headers["Content-Type"] = "application/json";
        Json::Value deleteBody;
        deleteBody["PersonId"] = account.personId;
        deleteBody["ForceDelete"] = true;
        deleteRequest.body = deleteBody.toStyledString();
        const auto [deleteResult, deleteResponse] = sendHttpRequest(
            "https://webapi.tobit.com", deleteRequest, 30.0);
        if (deleteResult != account::HttpResultCode::Ok || !deleteResponse ||
            deleteResponse->statusCode != 200) {
            return false;
        }

        account::HttpRequest invalidateRequest;
        invalidateRequest.method = account::HttpMethod::Post;
        invalidateRequest.path = "/v2/invalidToken";
        invalidateRequest.headers["Content-Type"] = "application/json; charset=utf-8";
        Json::Value invalidateBody;
        invalidateBody["token"] = account.authToken;
        invalidateRequest.body = invalidateBody.toStyledString();
        (void)sendHttpRequest("https://auth.tobit.com", invalidateRequest, 30.0);
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR << "[上游删除] 异常: " << error.what();
        return false;
    }
}

bool AccountManager::getUserProAccess(const string& token, const string& personId)
{
    if (token.empty() || personId.empty()) return false;
    account::HttpRequest request;
    request.method = account::HttpMethod::Get;
    request.path = "/ai-proxy/v1/userSettings/personId/" + personId;
    request.headers["Content-Type"] = "application/json";
    request.headers["Authorization"] = "Bearer " + token;
    try {
        const auto [result, response] = sendHttpRequest("https://cube.tobit.cloud", request, 30.0);
        if (result != account::HttpResultCode::Ok || !response || response->statusCode != 200) {
            return false;
        }
        Json::Value body;
        std::string errors;
        return account::workflow::parseJsonBody(response->body, body, errors) &&
               body.get("hasProAccess", false).asBool();
    } catch (const std::exception& error) {
        LOG_ERROR << "[账户管理] 查询 Pro 权限时捕获异常: " << error.what();
        return false;
    }
}

void AccountManager::updateAccountType(std::shared_ptr<Accountinfo_st> account)
{
    // Production historically disables active account-type probing.  Keep that
    // contract while making the health owner explicit; P8 can enable it behind
    // a policy/config switch without re-coupling it to selector state.
    if (account) {
        LOG_INFO << "[账户管理] 账号类型探测当前禁用: " << account->userName;
    }
}

void AccountManager::updateAllAccountTypes()
{
    std::vector<std::shared_ptr<Accountinfo_st>> accounts;
    for (const auto& [_, byUser] : getAccountList()) {
        for (const auto& [__, account] : byUser) {
            if (account && !account::workflow::shouldSkipLifecycleRefresh(account) &&
                account->tokenStatus && !account->authToken.empty()) {
                accounts.push_back(account);
            }
        }
    }
    for (const auto& account : accounts) {
        if (backgroundStopRequested_.load()) break;
        updateAccountType(account);
        if (!backgroundSleep(std::chrono::milliseconds(500))) break;
    }
}

void AccountManager::cleanExpiredAccounts()
{
    const auto settings = getAccountAutomationSettings();
    if (!settings.autoDeleteEnabled) return;

    const auto now = std::chrono::system_clock::now();
    const double defaultLifetime = static_cast<double>(settings.deleteAfterDays) * 24.0 * 3600.0;
    std::map<std::string, int> retentionDays;
    for (const auto& channel : requireChannelStore()->getChannelList()) {
        retentionDays[channel.channelName] = channel.accountRetentionDays;
    }

    std::vector<Accountinfo_st> expiredAccounts;
    for (const auto& [_, byUser] : getAccountList()) {
        for (const auto& [__, account] : byUser) {
            if (!account || account::workflow::shouldSkipLifecycleRefresh(account) ||
                account->createTime.empty() || account->accountType != "free") {
                continue;
            }
            const auto created = platform::parseLocalDbTimestamp(account->createTime);
            if (!created.has_value()) continue;
            const double age = std::chrono::duration<double>(now - *created).count();
            const auto channelIt = retentionDays.find(account->apiName);
            const int channelDays = channelIt == retentionDays.end() ? 0 : channelIt->second;
            if (age >= defaultLifetime ||
                (channelDays > 0 && age >= static_cast<double>(channelDays) * 24.0 * 3600.0)) {
                expiredAccounts.push_back(*account);
            }
        }
    }

    std::vector<std::string> expiredWorkspaces;
    const auto retoolIt = retentionDays.find("retoolapi");
    if (retoolIt != retentionDays.end() && retoolIt->second > 0 && workspaceUseCase_) {
        for (const auto& workspace : workspaceUseCase_->list()) {
            if (!account::workflow::isRetoolWorkspaceActive(workspace) || workspace.inUseCount > 0 ||
                workspace.createdAt.empty()) continue;
            const std::string reference = workspace.lastUsedAt.empty()
                ? workspace.createdAt : workspace.lastUsedAt;
            const auto date = platform::parseLocalDbTimestamp(reference);
            if (!date.has_value()) continue;
            const double age = std::chrono::duration<double>(now - *date).count();
            if (age >= static_cast<double>(retoolIt->second) * 24.0 * 3600.0) {
                expiredWorkspaces.push_back(workspace.workspaceId);
            }
        }
    }

    for (const auto& account : expiredAccounts) {
        if (!deleteAccountbyPost(account.apiName, account.userName)) continue;
        (void)deleteUpstreamAccount(account);
        (void)requireStore()->deleteAccount(account.apiName, account.userName);
    }
    for (const auto& id : expiredWorkspaces) {
        std::string error;
        if (!workspaceUseCase_->disable(id, &error)) {
            LOG_WARN << "[自动清理] Retool workspace 禁用失败: " << id << " error=" << error;
        }
    }
    if (!expiredAccounts.empty() || !expiredWorkspaces.empty()) {
        loadAccount();
        checkChannelAccountCounts();
    }
}
