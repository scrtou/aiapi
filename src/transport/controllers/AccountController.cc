#include <transport/controllers/AccountController.h>
#include <transport/controllers/codecs/AccountJsonCodec.h>
#include <transport/controllers/ControllerUtils.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <list>
#include <thread>

using namespace drogon;
using std::list;

IAccountAdminUseCase* AccountController::accounts_ = nullptr;

void AccountController::setUseCase(IAccountAdminUseCase* accounts)
{
    accounts_ = accounts;
}

namespace {

// 对外账号响应统一输出：仅保留 camelCase 新字段，彻底移除旧字段。
Json::Value buildAccountPublicJson(const Accountinfo_st& account)
{
    // This legacy admin endpoint historically returns credentials.  Keep that
    // wire contract explicit while the codec's safe default remains redacted.
    return accountcodec::toJson(account, true);
}

bool parseAccountPayload(const Json::Value& value,
                         Accountinfo_st& account,
                         std::string& errorMessage)
{
    if (!value.isObject()) {
        errorMessage = "Account item must be a JSON object.";
        return false;
    }
    if (value.isMember("workspaceUacId") && !value["workspaceUacId"].isIntegral()) {
        errorMessage = "workspaceUacId must be an integer.";
        return false;
    }

    account = accountcodec::fromJson(value);
    if (account.workspaceUacId < 0) {
        errorMessage = "workspaceUacId must be greater than or equal to 0.";
        return false;
    }

    const bool isChaynsPro =
        account.apiName == "chaynsapi" && account.accountType == "pro";
    if (isChaynsPro && account.workspaceUacId <= 0) {
        errorMessage = "Chayns Pro accounts require a positive workspaceUacId.";
        return false;
    }
    if (!isChaynsPro) {
        account.workspaceUacId = 0;
    }
    return true;
}

Json::Value buildAccountAutomationSettingsJson(const AccountAutomationSettings& settings)
{
    Json::Value item;
    item["autoDeleteEnabled"] = settings.autoDeleteEnabled;
    item["deleteAfterDays"] = settings.deleteAfterDays;
    item["autoRegisterEnabled"] = settings.autoRegisterEnabled;
    item["namespaceToolBridgeEnabled"] = settings.namespaceToolBridgeEnabled;
    return item;
}

bool mergeAccountAutomationSettingsFromJson(const Json::Value& body,
                                            const AccountAutomationSettings& current,
                                            AccountAutomationSettings& updated,
                                            std::string& errorMessage)
{
    if (!body.isObject()) {
        errorMessage = "Request body must be a JSON object.";
        return false;
    }

    updated = current;

    if (body.isMember("autoDeleteEnabled")) {
        if (!body["autoDeleteEnabled"].isBool()) {
            errorMessage = "autoDeleteEnabled must be a boolean.";
            return false;
        }
        updated.autoDeleteEnabled = body["autoDeleteEnabled"].asBool();
    }

    if (body.isMember("deleteAfterDays")) {
        if (!body["deleteAfterDays"].isInt()) {
            errorMessage = "deleteAfterDays must be an integer.";
            return false;
        }
        updated.deleteAfterDays = body["deleteAfterDays"].asInt();
    }

    if (body.isMember("autoRegisterEnabled")) {
        if (!body["autoRegisterEnabled"].isBool()) {
            errorMessage = "autoRegisterEnabled must be a boolean.";
            return false;
        }
        updated.autoRegisterEnabled = body["autoRegisterEnabled"].asBool();
    }

    if (body.isMember("namespaceToolBridgeEnabled")) {
        if (!body["namespaceToolBridgeEnabled"].isBool()) {
            errorMessage = "namespaceToolBridgeEnabled must be a boolean.";
            return false;
        }
        updated.namespaceToolBridgeEnabled = body["namespaceToolBridgeEnabled"].asBool();
    }

    if (updated.deleteAfterDays <= 0) {
        errorMessage = "deleteAfterDays must be greater than 0.";
        return false;
    }

    return true;
}

} // namespace

void AccountController::accountAdd(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[账号Ctrl] 添加账号";
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;

    Json::Value reqItems(Json::arrayValue);
    if (jsonPtr->isObject()) {
        reqItems.append(*jsonPtr);
    } else if (jsonPtr->isArray()) {
        reqItems = *jsonPtr;
    } else {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "Request body must be a JSON object or an array of objects.");
        return;
    }

    LOG_INFO << "[账号Ctrl] 开始添加账号";
    Json::Value response;
    list<Accountinfo_st> accountList;

    // 生成当前时间字符串
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_time_t), "%Y-%m-%d %H:%M:%S");
    std::string currentTime = ss.str();

    for (auto &item : reqItems)
    {
        Accountinfo_st accountinfo;
        std::string errorMessage;
        Json::Value responseitem;
        if (!parseAccountPayload(item, accountinfo, errorMessage)) {
            responseitem["apiName"] = item.get("apiName", "").asString();
            responseitem["userName"] = item.get("userName", "").asString();
            responseitem["status"] = "failed";
            responseitem["message"] = errorMessage;
            response.append(responseitem);
            continue;
        }
        accountinfo.createTime = currentTime;
        responseitem["apiName"] = accountinfo.apiName;
        responseitem["userName"] = accountinfo.userName;
        // 先添加到 账号Manager
        if (accounts_->stageAdd(accountinfo)) {
            responseitem["status"] = "success";
            accountList.push_back(accountinfo);
        } else {
            responseitem["status"] = "failed";
        }
        response.append(responseitem);
    }
    const auto addEnqueued = accounts_ ? accounts_->persistAdds(accountList) : TaskSubmitResult::Stopped;
    if (addEnqueued != TaskSubmitResult::Accepted) {
        // 任务未入队 = 上述工作一件也不会发生，不能再回 success/started。
        LOG_WARN << "[账号Ctrl] 添加账号 后台任务入队被拒：" << toString(addEnqueued);
        ctl::sendError(callback, k503ServiceUnavailable, "service_unavailable",
                       std::string("Background task rejected: ") + toString(addEnqueued));
        return;
    }

    ctl::sendJson(callback, response);
    LOG_INFO << "[账号Ctrl] 添加账号完成";
}

void AccountController::accountInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[账号Ctrl] 获取账号信息";
    auto accountList = accounts_ ? accounts_->listAccounts() : IAccountAdminUseCase::AccountMap{};
    Json::Value response(Json::arrayValue);
    for (auto &account : accountList) {
        for (auto &userName : account.second) {
            if (!userName.second) {
                continue;
            }
            if (userName.second->status == AccountStatus::WAITING ||
                userName.second->status == AccountStatus::REGISTERING) {
                continue;
            }
            response.append(buildAccountPublicJson(*userName.second));
        }
    }
    if (response.empty()) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k200OK);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody("[]");
        callback(resp);
    } else {
        ctl::sendJson(callback, response);
    }
}

void AccountController::accountBackupInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[账号Ctrl] 获取备份账号信息";
    Json::Value response(Json::arrayValue);
    for (auto &account : accounts_ ? accounts_->listBackupAccounts() : std::list<Accountinfo_st>{}) {
        response.append(buildAccountPublicJson(account));
    }
    if (response.empty()) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k200OK);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody("[]");
        callback(resp);
    } else {
        ctl::sendJson(callback, response);
    }
}

void AccountController::accountDelete(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[账号Ctrl] 删除账号";
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;

    Json::Value reqItems(Json::arrayValue);
    if (jsonPtr->isObject()) {
        reqItems.append(*jsonPtr);
    } else if (jsonPtr->isArray()) {
        reqItems = *jsonPtr;
    } else {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "Request body must be a JSON object or an array of objects.");
        return;
    }

    Json::Value response;
    list<Accountinfo_st> accountList;

    // 在删除前获取完整账号信息（包含 authToken、 等），用于上游删除
    auto currentAccountMap = accounts_ ? accounts_->listAccounts() : IAccountAdminUseCase::AccountMap{};

    for (auto &item : reqItems)
    {
        Accountinfo_st accountinfo;
        Json::Value responseitem;
        accountinfo.apiName = item["apiName"].asString();
        accountinfo.userName = item["userName"].asString();
        responseitem["apiName"] = accountinfo.apiName;
        responseitem["userName"] = accountinfo.userName;

        // 检查账号是否正在注册中，如果是则拒绝删除
        if (accounts_->isRegistering(accountinfo.userName)) {
            responseitem["status"] = "failed";
            responseitem["error"] = "Account is currently being registered, cannot delete";
            LOG_WARN << "[账号Ctrl] 账号" << accountinfo.userName << " 正在注册中，无法删除";
            response.append(responseitem);
            continue;
        }

        // 在从内存删除前，获取完整的账号信息用于上游删除
        if (currentAccountMap.find(accountinfo.apiName) != currentAccountMap.end() &&
            currentAccountMap[accountinfo.apiName].find(accountinfo.userName) != currentAccountMap[accountinfo.apiName].end()) {
            auto fullAccount = currentAccountMap[accountinfo.apiName][accountinfo.userName];
            accountinfo.passwd = fullAccount->passwd;
            accountinfo.authToken = fullAccount->authToken;
            accountinfo.userTobitId = fullAccount->userTobitId;
            accountinfo.personId = fullAccount->personId;
        }

        if (accounts_->stageDelete(accountinfo.apiName, accountinfo.userName)) {
            responseitem["status"] = "success";
            accountList.push_back(accountinfo);
        } else {
            responseitem["status"] = "failed";
        }
        response.append(responseitem);
    }
    const auto deleteEnqueued = accounts_ ? accounts_->persistDeletes(accountList) : TaskSubmitResult::Stopped;
    if (deleteEnqueued != TaskSubmitResult::Accepted) {
        // 任务未入队 = 上述工作一件也不会发生，不能再回 success/started。
        LOG_WARN << "[账号Ctrl] 删除账号 后台任务入队被拒：" << toString(deleteEnqueued);
        ctl::sendError(callback, k503ServiceUnavailable, "service_unavailable",
                       std::string("Background task rejected: ") + toString(deleteEnqueued));
        return;
    }

    ctl::sendJson(callback, response);
}

void AccountController::accountDbInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[账号Ctrl] 获取账号数据库信息";
    Json::Value response;
    response["dbName"] = "aichat";
    response["tableName"] = "account";
    for (auto &account : accounts_ ? accounts_->listStoredAccounts() : std::list<Accountinfo_st>{}) {
        response.append(buildAccountPublicJson(account));
    }
    ctl::sendJson(callback, response);
}

void AccountController::accountUpdate(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[账号Ctrl] 更新账号";
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;

    Json::Value reqItems(Json::arrayValue);
    if (jsonPtr->isObject()) {
        reqItems.append(*jsonPtr);
    } else if (jsonPtr->isArray()) {
        reqItems = *jsonPtr;
    } else {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "Request body must be a JSON object or an array of objects.");
        return;
    }

    Json::Value response;
    list<Accountinfo_st> accountList;

    for (auto &item : reqItems)
    {
        Accountinfo_st accountinfo;
        std::string errorMessage;
        Json::Value responseitem;
        if (!parseAccountPayload(item, accountinfo, errorMessage)) {
            responseitem["apiName"] = item.get("apiName", "").asString();
            responseitem["userName"] = item.get("userName", "").asString();
            responseitem["status"] = "failed";
            responseitem["message"] = errorMessage;
            response.append(responseitem);
            continue;
        }

        responseitem["apiName"] = accountinfo.apiName;
        responseitem["userName"] = accountinfo.userName;

        if (accounts_->stageUpdate(accountinfo)) {
            responseitem["status"] = "success";
            accountList.push_back(accountinfo);
        } else {
            responseitem["status"] = "failed";
            responseitem["message"] = "Account not found";
        }
        response.append(responseitem);
    }

    const auto updateEnqueued = accounts_ ? accounts_->persistUpdates(accountList) : TaskSubmitResult::Stopped;
    if (updateEnqueued != TaskSubmitResult::Accepted) {
        // 任务未入队 = 上述工作一件也不会发生，不能再回 success/started。
        LOG_WARN << "[账号Ctrl] 更新账号 后台任务入队被拒：" << toString(updateEnqueued);
        ctl::sendError(callback, k503ServiceUnavailable, "service_unavailable",
                       std::string("Background task rejected: ") + toString(updateEnqueued));
        return;
    }


    ctl::sendJson(callback, response);
    LOG_INFO << "[账号Ctrl] 更新账号完成";
}

void AccountController::accountRefresh(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[账号Ctrl] 刷新账号状态（token有效性+账号类型）";

    Json::Value response;
    response["status"] = "started";
    response["message"] = "Account status refresh started in background";

    // 异步执行刷新操作
    const auto refreshEnqueued = accounts_ ? accounts_->refreshAccounts() : TaskSubmitResult::Stopped;
    if (refreshEnqueued != TaskSubmitResult::Accepted) {
        // 任务未入队 = 上述工作一件也不会发生，不能再回 success/started。
        LOG_WARN << "[账号Ctrl] 刷新状态 后台任务入队被拒：" << toString(refreshEnqueued);
        ctl::sendError(callback, k503ServiceUnavailable, "service_unavailable",
                       std::string("Background task rejected: ") + toString(refreshEnqueued));
        return;
    }


    ctl::sendJson(callback, response);
}

void AccountController::accountAutoRegister(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[账号Ctrl] 自动注册账号";
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;

    auto& reqBody = *jsonPtr;
    std::string apiName = reqBody.get("apiName", "chaynsapi").asString();
    int count = reqBody.get("count", 1).asInt();

    const auto channelEnabled = accounts_ ? accounts_->channelEnabled(apiName) : std::nullopt;
    if (!channelEnabled.has_value())
    {
        ctl::sendError(callback, k404NotFound, "not_found", "channel not found");
        return;
    }
    if (!*channelEnabled)
    {
        ctl::sendError(callback, k409Conflict, "channel_disabled", "channel is disabled");
        return;
    }

    // 限制一次最多注册 20 个
    if (count < 1) count = 1;
    if (count > 20) count = 20;

    LOG_INFO << "[账号Ctrl] 自动注册参数：apiName=" << apiName << ", count=" << count;

    Json::Value response;
    response["status"] = "started";
    response["message"] = "Auto registration started in background";
    response["apiName"] = apiName;
    response["count"] = count;

    // 异步执行注册操作
    const auto registerEnqueued = accounts_ ? accounts_->autoRegister(apiName, count) : TaskSubmitResult::Stopped;
    if (registerEnqueued != TaskSubmitResult::Accepted) {
        // 任务未入队 = 上述工作一件也不会发生，不能再回 success/started。
        LOG_WARN << "[账号Ctrl] 自动注册 后台任务入队被拒：" << toString(registerEnqueued);
        ctl::sendError(callback, k503ServiceUnavailable, "service_unavailable",
                       std::string("Background task rejected: ") + toString(registerEnqueued));
        return;
    }


    ctl::sendJson(callback, response);
}

void AccountController::accountSettingsGet(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    (void)req;
    LOG_INFO << "[账号Ctrl] 获取账号自动化设置";
    const auto settings = accounts_->automationSettings();
    ctl::sendJson(callback, buildAccountAutomationSettingsJson(settings));
}

void AccountController::accountSettingsUpdate(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[账号Ctrl] 更新账号自动化设置";
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;

    const auto currentSettings = accounts_->automationSettings();
    AccountAutomationSettings updatedSettings;
    std::string errorMessage;
    if (!mergeAccountAutomationSettingsFromJson(*jsonPtr, currentSettings, updatedSettings, errorMessage)) {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", errorMessage);
        return;
    }

    if (!accounts_->updateAutomationSettings(updatedSettings, &errorMessage)) {
        ctl::sendError(callback, k500InternalServerError, "config_update_error", errorMessage.empty() ? "Failed to update account automation settings." : errorMessage);
        return;
    }

    Json::Value response;
    response["status"] = "success";
    response["message"] = "Account automation settings updated";
    response["settings"] = buildAccountAutomationSettingsJson(updatedSettings);
    ctl::sendJson(callback, response);
}
