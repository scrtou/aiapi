#include <accountManager/accountManager.h>

#include <accountManager/AccountWorkflowSupport.h>
#include <accountManager/RetoolProvisionHealth.h>
#include <domain/policy/RetiredProviderPolicy.h>
#include <accountManager/LoginResponseLogSummary.h>
#include <platform/Log.h>

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

std::string randomLetters(int length)
{
    static constexpr char kCharacters[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string value;
    value.reserve(static_cast<size_t>(length));
    for (int i = 0; i < length; ++i) {
        value += kCharacters[std::rand() % (sizeof(kCharacters) - 1)];
    }
    return value;
}

Json::Value buildRegistrationWorkflowBody(const std::string& firstName,
                                          const std::string& lastName,
                                          const std::string& password)
{
    Json::Value body;
    body["mail_policy"]["expiry_time_ms"] = 3600000;
    body["proxy_policy"]["enabled"] = false;
    body["identity"]["first_name"] = firstName;
    body["identity"]["last_name"] = lastName;
    body["identity"]["password"] = password;
    body["site"] = "chayns";
    body["strategy"]["registration_mode"] = "api_first";
    body["strategy"]["login_mode"] = "api_first";
    body["strategy"]["timeout_seconds"] = 360;
    return body;
}

std::string workflowDetailPath(std::string createPath, const std::string& taskId)
{
    static const std::string kCreateSuffix = "/register-and-login";
    if (createPath.size() >= kCreateSuffix.size() &&
        createPath.compare(createPath.size() - kCreateSuffix.size(),
                           kCreateSuffix.size(), kCreateSuffix) == 0) {
        createPath.resize(createPath.size() - kCreateSuffix.size());
    }
    if (createPath.empty()) {
        createPath = "/api/v1/workflows";
    }
    return createPath + "/" + taskId;
}

class RegistrationScope
{
  public:
    RegistrationScope(account::AccountRegistrationStateMachine& state, int waitingId)
        : state_(state), waitingId_(waitingId)
    {
    }
    ~RegistrationScope() { state_.finish(waitingId_); }

  private:
    account::AccountRegistrationStateMachine& state_;
    int waitingId_;
};

}  // namespace

void AccountManager::rollbackWaitingAccount(int waitingId)
{
    if (registrationStateMachine_) {
        registrationStateMachine_->rollback(waitingId);
    }
}

bool AccountManager::autoRegisterAccount(string apiName)
{
    LOG_INFO << "[自动注册] 开始为渠道 " << apiName << " 自动注册账号";
    if (retired_provider::isRetiredProviderKey(apiName)) {
        LOG_ERROR << "[自动注册] 已退役 Provider 禁止自动注册: " << apiName;
        return false;
    }
    for (const auto& channel : requireChannelStore()->getChannelList()) {
        if (channel.channelName == apiName && !channel.channelStatus) {
            LOG_WARN << "[自动注册] 渠道已禁用，跳过自动注册: " << apiName;
            return false;
        }
    }

    if (apiName == "retoolapi") {
        if (!retoolProvisionClock_) {
            LOG_ERROR << "[自动注册] Retool provision clock 未注入";
            return false;
        }
        const auto health = retoolProvision::loadRetoolProvisionHealth(*configStore_, nullptr);
        if (retoolProvision::isRetoolProvisionCoolingDown(health, *retoolProvisionClock_)) {
            LOG_WARN << "[自动注册] Retool 渠道处于冷却期，跳过自动注册。cooldownUntil="
                     << health.cooldownUntil << " reason=" << health.lastFailureReason;
            return false;
        }

        Json::Value requestBody(Json::objectValue);
        requestBody["mail_providers"] = Json::Value(Json::arrayValue);
        requestBody["mail_providers"].append("gptmail");
        requestBody["password"] = "RetoolFlow123!!";
        requestBody["full_name"] = "Codex Flow";
        requestBody["workspace_prefix"] = "codexorg";
        try {
            if (!workspaceProvisioner_) {
                throw std::runtime_error("Retool workspace provisioner 未注入");
            }
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            const auto workspace = workspaceProvisioner_->provision(
                Json::writeString(writer, requestBody));
            if (workspace.workspaceId.empty()) {
                retoolProvision::markRetoolProvisionFailure(
                    *configStore_, "unknown error", *retoolProvisionClock_);
                return false;
            }
            retoolProvision::markRetoolProvisionSuccess(*configStore_);
            LOG_INFO << "[自动注册] Retool workspace 创建成功: " << workspace.workspaceId;
            return true;
        } catch (const std::exception& error) {
            retoolProvision::markRetoolProvisionFailure(
                *configStore_, error.what(), *retoolProvisionClock_);
            LOG_ERROR << "[自动注册] Retool workspace 创建异常: " << error.what();
            return false;
        }
    }

    if (!registrationStateMachine_) {
        LOG_ERROR << "[自动注册] registration state machine 未初始化";
        return false;
    }
    const int waitingId = registrationStateMachine_->begin(apiName);
    if (waitingId < 0) {
        LOG_ERROR << "[自动注册] 创建或进入 registering 状态失败: " << apiName;
        return false;
    }
    RegistrationScope scope(*registrationStateMachine_, waitingId);

    const std::string firstName = "User" + randomLetters(5);
    const std::string lastName = "Auto" + randomLetters(5);
    const std::string password = "Pwd" + randomLetters(8) + "!";
    const std::string fullUrl = account::workflow::registrationServiceUrl(runtimeConfig_, apiName);
    if (fullUrl.empty()) {
        rollbackWaitingAccount(waitingId);
        return false;
    }

    std::string baseUrl;
    std::string path;
    if (!account::workflow::splitUrl(fullUrl, baseUrl, path)) {
        LOG_ERROR << "[自动注册] 无效的注册服务URL格式: " << fullUrl;
        rollbackWaitingAccount(waitingId);
        return false;
    }

    account::HttpRequest request;
    request.method = account::HttpMethod::Post;
    request.path = path;
    request.headers["Content-Type"] = "application/json";
    request.body = buildRegistrationWorkflowBody(firstName, lastName, password).toStyledString();
    const auto downstreamApiKey = account::workflow::downstreamBearerApiKey(runtimeConfig_, apiName);
    if (!downstreamApiKey.empty()) {
        request.headers["Authorization"] = "Bearer " + downstreamApiKey;
    }

    auto [result, response] = sendHttpRequest(baseUrl, request, 300.0);
    if (result != account::HttpResultCode::Ok || !response || response->statusCode != 200) {
        rollbackWaitingAccount(waitingId);
        return false;
    }

    Json::Value created;
    std::string parseErrors;
    const std::string createdBody(response->body);
    if (!account::workflow::parseJsonBody(createdBody, created, parseErrors) ||
        !account::workflow::isSuccessEnvelope(created)) {
        LOG_ERROR << "[自动注册] workflow 创建响应无效: "
                  << account_logging::summarizeParseError(parseErrors);
        rollbackWaitingAccount(waitingId);
        return false;
    }
    const std::string taskId = created["data"].get("task_id", "").asString();
    if (taskId.empty()) {
        rollbackWaitingAccount(waitingId);
        return false;
    }

    Json::Value workflowDetail;
    bool succeeded = false;
    constexpr int kWorkflowPollAttempts = 300;
    for (int attempt = 0; attempt < kWorkflowPollAttempts; ++attempt) {
        account::HttpRequest detailRequest;
        detailRequest.method = account::HttpMethod::Get;
        detailRequest.path = workflowDetailPath(path, taskId);
        if (!downstreamApiKey.empty()) {
            detailRequest.headers["Authorization"] = "Bearer " + downstreamApiKey;
        }
        auto [detailResult, detailResponse] = sendHttpRequest(baseUrl, detailRequest, 30.0);
        if (detailResult != account::HttpResultCode::Ok || !detailResponse) {
            clock_->sleepFor(std::chrono::seconds(3));
            continue;
        }
        Json::Value detail;
        parseErrors.clear();
        if (!account::workflow::parseJsonBody(detailResponse->body, detail,
                                               parseErrors) ||
            !account::workflow::isSuccessEnvelope(detail) || !detail["data"].isObject() ||
            !detail["data"].isMember("task")) {
            clock_->sleepFor(std::chrono::seconds(3));
            continue;
        }
        workflowDetail = detail;
        const std::string status = detail["data"]["task"].get("status", "").asString();
        if (status == "succeeded") {
            succeeded = true;
            break;
        }
        if (status == "failed" || status == "cancelled") {
            break;
        }
        clock_->sleepFor(std::chrono::seconds(3));
    }

    if (!succeeded) {
        LOG_ERROR << "[自动注册] workflow 未成功完成: "
                  << account_logging::summarizeWorkflowEnvelope(workflowDetail);
        rollbackWaitingAccount(waitingId);
        return false;
    }

    const auto& resultJson = workflowDetail["data"]["result"];
    const auto& registrationJson = resultJson["registration"];
    const auto& loginJson = resultJson["login"];
    const std::string email = registrationJson["account"].get("email", "").asString();
    const std::string responsePassword = registrationJson["account"].get("password", password).asString();
    const std::string token = loginJson["session"].get("access_token", "").asString();
    const std::string personId = loginJson["site_result"].get("personid", "").asString();
    if (email.empty() || personId.empty() || token.empty()) {
        rollbackWaitingAccount(waitingId);
        return false;
    }

    const bool hasProAccess = loginJson.isMember("site_result") &&
        loginJson["site_result"].get("has_pro_access", false).asBool();
    Accountinfo_st account(apiName, email, responsePassword, token, 0, true, true,
                           loginJson["site_result"].get("userid", 0).asInt(), personId,
                           account::workflow::currentLocalDbTimestamp(),
                           hasProAccess ? "pro" : "free", AccountStatus::ACTIVE);
    if (!registrationStateMachine_->activate(waitingId, account)) {
        rollbackWaitingAccount(waitingId);
        return false;
    }
    addAccount(account.apiName, account.userName, account.passwd, account.authToken,
               account.useCount, account.tokenStatus, account.accountStatus,
               account.userTobitId, account.personId, account.createTime,
               account.accountType, account.status, account.workspaceUacId);
    return true;
}

void AccountManager::autoRegisterAccounts(std::string apiName, int count)
{
    for (int index = 0; index < count; ++index) {
        if (!autoRegisterAccount(apiName)) break;
        if (index + 1 < count && !backgroundSleep(std::chrono::seconds(5))) break;
    }
}

bool AccountManager::isAccountRegistering(int pendingId)
{
    return registrationStateMachine_ && registrationStateMachine_->isRegistering(pendingId);
}

bool AccountManager::isAccountRegisteringByUsername(const string& userName)
{
    return registrationStateMachine_ &&
           registrationStateMachine_->isRegisteringByUsername("chaynsapi", userName);
}
