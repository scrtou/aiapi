#include "AccountController.h"
#include "ControllerUtils.h"
#include <accountManager/accountManager.h>
#include <dbManager/account/accountDbManager.h>
#include <utils/BackgroundTaskQueue.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <list>
#include <thread>

using namespace drogon;
using std::list;

namespace {

// 对外账号响应统一输出：仅保留 camelCase 新字段，彻底移除旧字段。
Json::Value buildAccountPublicJson(const Accountinfo_st& account)
{
    Json::Value item;
    item["apiName"] = account.apiName;
    item["userName"] = account.userName;
    item["password"] = account.passwd;
    item["authToken"] = account.authToken;
    item["useCount"] = account.useCount;
    item["tokenStatus"] = account.tokenStatus;
    item["accountStatus"] = account.accountStatus;
    item["userTobitId"] = account.userTobitId;
    item["personId"] = account.personId;
    item["createTime"] = account.createTime;
    item["accountType"] = account.accountType;
    item["status"] = account.status;
    return item;
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
        Accountinfo_st accountinfo = Accountinfo_st::fromJson(item);
        accountinfo.createTime = currentTime;
        Json::Value responseitem;
        responseitem["apiName"] = accountinfo.apiName;
        responseitem["userName"] = accountinfo.userName;
        // 先添加到 账号Manager
        if (AccountManager::getInstance().addAccountbyPost(accountinfo)) {
            responseitem["status"] = "success";
            accountList.push_back(accountinfo);
        } else {
            responseitem["status"] = "failed";
        }
        response.append(responseitem);
    }
    BackgroundTaskQueue::instance().enqueue("accountAdd", [accountList](){
        for (auto &account : accountList) {
            AccountDbManager::getInstance()->addAccount(account);
        }
        AccountManager::getInstance().checkUpdateAccountToken();
        // 账号添加后，只对新添加的账号更新 账号Type
        for (const auto &account : accountList) {
            auto accountMap = AccountManager::getInstance().getAccountList();
            if (accountMap.find(account.apiName) != accountMap.end() &&
                accountMap[account.apiName].find(account.userName) != accountMap[account.apiName].end()) {
                AccountManager::getInstance().updateAccountType(accountMap[account.apiName][account.userName]);
            }
        }
    });
    ctl::sendJson(callback, response);
    LOG_INFO << "[账号Ctrl] 添加账号完成";
}

void AccountController::accountInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[账号Ctrl] 获取账号信息";
    auto accountList = AccountManager::getInstance().getAccountList();
    Json::Value response(Json::arrayValue);
    for (auto &account : accountList) {
        for (auto &userName : account.second) {
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
    auto currentAccountMap = AccountManager::getInstance().getAccountList();

    for (auto &item : reqItems)
    {
        Accountinfo_st accountinfo;
        Json::Value responseitem;
        accountinfo.apiName = item["apiName"].asString();
        accountinfo.userName = item["userName"].asString();
        responseitem["apiName"] = accountinfo.apiName;
        responseitem["userName"] = accountinfo.userName;

        // 检查账号是否正在注册中，如果是则拒绝删除
        if (AccountManager::getInstance().isAccountRegisteringByUsername(accountinfo.userName)) {
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

        if (AccountManager::getInstance().deleteAccountbyPost(accountinfo.apiName, accountinfo.userName)) {
            responseitem["status"] = "success";
            accountList.push_back(accountinfo);
        } else {
            responseitem["status"] = "failed";
        }
        response.append(responseitem);
    }
    BackgroundTaskQueue::instance().enqueue("accountDelete", [accountList](){
        for (auto &account : accountList) {
            // 先从上游删除账号
            bool upstreamDeleted = AccountManager::getInstance().deleteUpstreamAccount(account);
            if (upstreamDeleted) {
                LOG_INFO << "[账号Ctrl] 上游账号删除成功：" << account.userName;
            } else {
                LOG_WARN << "[账号Ctrl] 上游账号删除失败（继续删除本地数据库）：" << account.userName;
            }
            // 再从本地数据库删除
            AccountDbManager::getInstance()->deleteAccount(account.apiName, account.userName);
        }
        AccountManager::getInstance().loadAccount();
        // 账号删除后，检查渠道账号数量（可能需要补充账号）
        AccountManager::getInstance().checkChannelAccountCounts();
    });
    ctl::sendJson(callback, response);
}

void AccountController::accountDbInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[账号Ctrl] 获取账号数据库信息";
    Json::Value response;
    response["dbName"] = "aichat";
    response["tableName"] = "account";
    for (auto &account : AccountDbManager::getInstance()->getAccountDBList()) {
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
        Accountinfo_st accountinfo = Accountinfo_st::fromJson(item);

        Json::Value responseitem;
        responseitem["apiName"] = accountinfo.apiName;
        responseitem["userName"] = accountinfo.userName;

        if (AccountManager::getInstance().updateAccount(accountinfo)) {
            responseitem["status"] = "success";
            accountList.push_back(accountinfo);
        } else {
            responseitem["status"] = "failed";
            responseitem["message"] = "Account not found";
        }
        response.append(responseitem);
    }

    BackgroundTaskQueue::instance().enqueue("accountUpdate", [accountList](){
        for (auto &account : accountList) {
            AccountDbManager::getInstance()->updateAccount(account);
        }
        // 账号更新后，只对操作的账号更新 账号Type
        for (const auto &account : accountList) {
            auto accountMap = AccountManager::getInstance().getAccountList();
            if (accountMap.find(account.apiName) != accountMap.end() &&
                accountMap[account.apiName].find(account.userName) != accountMap[account.apiName].end()) {
                AccountManager::getInstance().updateAccountType(accountMap[account.apiName][account.userName]);
            }
        }
    });

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
    BackgroundTaskQueue::instance().enqueue("accountRefresh", [](){
        LOG_INFO << "[账号Ctrl] 后台刷新：开始检查 有效性";
        AccountManager::getInstance().checkToken();
        LOG_INFO << "[账号Ctrl] 后台刷新：开始更新账号类型";
        AccountManager::getInstance().updateAllAccountTypes();
        LOG_INFO << "[账号Ctrl] 后台刷新：刷新完成";
    });

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
    BackgroundTaskQueue::instance().enqueue("accountAutoRegister", [apiName, count](){
        LOG_INFO << "[账号Ctrl] 后台注册：开始为" << apiName << " 注册 " << count << " 个账号";
        for (int i = 0; i < count; ++i) {
            LOG_INFO << "[账号Ctrl] 后台注册：正在注册第" << (i + 1) << "/" << count << " 个账号";
            AccountManager::getInstance().autoRegisterAccount(apiName);
            // 注册间隔 5 秒，避免过快
            if (i < count - 1) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
        LOG_INFO << "[账号Ctrl] 后台注册：注册完成";
    });

    ctl::sendJson(callback, response);
}
