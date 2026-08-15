#include <application/account/accountManager.h>

#include <application/account/AccountWorkflowSupport.h>
#include <application/account/LoginResponseLogSummary.h>
#include <platform/Log.h>

#include <chrono>
#include <vector>

bool AccountManager::checkChaynsToken(string token)
{
    LOG_INFO << "[账户管理] 开始校验 Chayns 令牌";
    account::HttpRequest request;
    request.method = account::HttpMethod::Get;
    request.path = "/AccountService/v1.0/Chayns/User";
    request.headers["Authorization"] = "Bearer " + token;
    auto [result, response] = sendHttpRequest(
        "https://webapi.tobit.com/AccountService/v1.0/Chayns/User", request, 30.0);
    if (result != account::HttpResultCode::Ok || !response) {
        LOG_ERROR << "[账户管理] 令牌校验请求失败, result=" << static_cast<int>(result);
        return false;
    }
    return response->statusCode == 200;
}

Json::Value AccountManager::getChaynsToken(string username, string passwd)
{
    const string fullUrl = account::workflow::loginServiceUrl(runtimeConfig_, "chaynsapi");
    if (fullUrl.empty()) {
        LOG_ERROR << "[账户管理] 配置中未找到 chaynsapi 的登录服务地址";
        return {};
    }

    string baseUrl;
    string path;
    if (!account::workflow::splitUrl(fullUrl, baseUrl, path)) {
        LOG_ERROR << "[账户管理] 登录服务地址格式无效: " << fullUrl;
        return {};
    }
    if (!isServerReachable(baseUrl)) {
        LOG_ERROR << "[账户管理] 达到最大重试次数后仍无法连通目标主机: " << baseUrl;
        return {};
    }

    account::HttpRequest request;
    Json::Value payload;
    payload["site"] = "chayns";
    payload["credentials"]["email"] = username;
    payload["credentials"]["password"] = passwd;
    payload["strategy"]["mode"] = "api_first";
    request.method = account::HttpMethod::Post;
    request.path = path;
    request.headers["Content-Type"] = "application/json";
    request.body = payload.toStyledString();
    const auto downstreamApiKey = account::workflow::downstreamBearerApiKey(
        runtimeConfig_, "chaynsapi");
    if (!downstreamApiKey.empty()) {
        request.headers["Authorization"] = "Bearer " + downstreamApiKey;
    }

    auto [result, response] = sendHttpRequest(baseUrl, request, 300.0);
    if (result != account::HttpResultCode::Ok || !response) {
        LOG_ERROR << "[账户管理] 登录服务请求失败, result=" << static_cast<int>(result);
        return {};
    }

    const int httpStatus = response->statusCode;
    const std::string body = httpStatus == 200 ? response->body : "";
    Json::Value envelope;
    string errors;
    if (!account::workflow::parseJsonBody(body, envelope, errors)) {
        LOG_ERROR << "[账户管理] 解析登录响应 JSON 失败: "
                  << account_logging::summarizeLoginTransport(
                         httpStatus, response->header("content-type"), body.size())
                  << ", " << account_logging::summarizeParseError(errors);
        return {};
    }
    if (!account::workflow::isSuccessEnvelope(envelope)) {
        LOG_ERROR << "[账户管理] 登录服务返回失败: "
                  << account_logging::summarizeLoginTransport(
                         httpStatus, response->header("content-type"), body.size())
                  << ", " << account_logging::summarizeLoginError(envelope);
        return {};
    }

    const auto& resultJson = envelope["data"]["result"];
    if (!resultJson.isObject()) {
        LOG_ERROR << "[账户管理] 登录服务响应缺少 data.result";
        return {};
    }
    Json::Value normalized;
    normalized["token"] = resultJson["session"].get("access_token", "").asString();
    normalized["userid"] = resultJson["site_result"].get("userid", 0).asInt();
    normalized["personid"] = resultJson["site_result"].get("personid", "").asString();
    normalized["email"] = resultJson["account"].get("email", username).asString();
    normalized["password"] = passwd;
    normalized["has_pro_access"] = resultJson["site_result"].get("has_pro_access", false).asBool();
    if (normalized["token"].asString().empty() || normalized["personid"].asString().empty()) {
        LOG_ERROR << "[账户管理] 登录服务响应缺少关键字段";
        return {};
    }
    return normalized;
}

void AccountManager::checkToken()
{
    LOG_INFO << "[账户管理] 开始批量校验账号令牌";
    std::vector<std::shared_ptr<Accountinfo_st>> accounts;
    for (const auto& [_, byUser] : getAccountList()) {
        for (const auto& [__, account] : byUser) {
            if (account) accounts.push_back(account);
        }
    }

    for (const auto& account : accounts) {
        if (account::workflow::shouldSkipLifecycleRefresh(account)) {
            continue;
        }
        const auto check = checkTokenMap.find(account->apiName);
        if (check == checkTokenMap.end()) {
            LOG_ERROR << "[账户管理] 不支持的上游渠道 apiName: " << account->apiName;
            continue;
        }
        const bool valid = (this->*(check->second))(account->authToken);
        setStatusTokenStatus(account->apiName, account->userName, valid);
    }
    LOG_INFO << "[账户管理] 批量令牌校验结束";
}

void AccountManager::updateToken()
{
    LOG_INFO << "[账户管理] 开始批量更新令牌";
    std::vector<std::shared_ptr<Accountinfo_st>> accounts;
    for (const auto& [_, byUser] : getAccountList()) {
        for (const auto& [__, account] : byUser) {
            if (account && !account::workflow::shouldSkipLifecycleRefresh(account) &&
                (!account->tokenStatus || account->authToken.empty())) {
                accounts.push_back(account);
            }
        }
    }

    for (const auto& account : accounts) {
        const auto update = updateTokenMap.find(account->apiName);
        if (update == updateTokenMap.end()) {
            LOG_ERROR << "[账户管理] 不支持的上游渠道 apiName: " << account->apiName;
            continue;
        }
        (this->*(update->second))(account);
        if (account->tokenStatus) {
            if (!requireStore()->updateAccount(*account)) {
                LOG_ERROR << "[账户管理] 更新数据库账号记录失败";
                continue;
            }
            refreshAccountQueue(account->apiName);
        }
    }
    LOG_INFO << "[账户管理] 批量令牌更新结束";
}

void AccountManager::updateChaynsToken(std::shared_ptr<Accountinfo_st> accountinfo)
{
    if (!accountinfo) return;
    LOG_INFO << "[账户管理] 开始更新 Chayns 令牌，用户: " << accountinfo->userName;
    const auto token = getChaynsToken(accountinfo->userName, accountinfo->passwd);
    if (token.empty()) {
        return;
    }
    accountinfo->tokenStatus = true;
    accountinfo->authToken = token["token"].asString();
    accountinfo->accountStatus = true;
    accountinfo->useCount = 0;
    accountinfo->userTobitId = token["userid"].asInt();
    accountinfo->personId = token["personid"].asString();
    accountinfo->accountType = token.get("has_pro_access", false).asBool() ? "pro" : "free";
}

void AccountManager::checkUpdateAccountToken()
{
    checkToken();
}

bool AccountManager::isServerReachable(const string& host, int maxRetries)
{
    const std::vector<std::string> candidatePaths = {"/api/v1/health", "/health", "/"};
    int retryCount = 0;
    while (retryCount < maxRetries && !backgroundStopRequested_.load()) {
        try {
            for (const auto& path : candidatePaths) {
                account::HttpRequest request;
                request.method = account::HttpMethod::Get;
                request.path = path;
                const auto [_, response] = sendHttpRequest(host, request, 30.0);
                if (response && response->statusCode == 200) {
                    return true;
                }
            }
        } catch (...) {
            LOG_INFO << "[账户管理] 目标主机暂不可达，准备重试: " << retryCount + 1;
        }
        ++retryCount;
        if (!backgroundSleep(std::chrono::seconds(1))) {
            return false;
        }
    }
    return false;
}

void AccountManager::waitUpdateAccountToken()
{
    LOG_INFO << "[账户管理] 账号令牌更新线程已启动";
    while (!backgroundStopRequested_.load()) {
        std::shared_ptr<Accountinfo_st> account;
        {
            std::unique_lock<std::mutex> lock(accountListNeedUpdateMutex);
            accountListNeedUpdateCondition.wait(lock, [this] {
                return !accountListNeedUpdate.empty() || backgroundStopRequested_.load();
            });
            if (backgroundStopRequested_.load()) break;
            account = accountListNeedUpdate.front();
            accountListNeedUpdate.pop_front();
        }
        if (!account || account::workflow::shouldSkipLifecycleRefresh(account)) {
            continue;
        }
        const auto update = updateTokenMap.find(account->apiName);
        if (update == updateTokenMap.end()) {
            LOG_ERROR << "[账户管理] 不支持的 API 名称: " << account->apiName;
            continue;
        }
        try {
            (this->*(update->second))(account);
            if (!account->tokenStatus) continue;
            if (!requireStore()->updateAccount(*account)) {
                LOG_ERROR << "[账户管理] 更新数据库账号记录失败";
                continue;
            }
            refreshAccountQueue(account->apiName);
        } catch (const std::exception& error) {
            LOG_ERROR << "[账户管理] 执行账号更新时发生异常: " << error.what();
        }
    }
    LOG_INFO << "[账户管理] 账号令牌更新线程已退出";
}
