#include "HealthController.h"
#include <apiManager/ApiManager.h>
#include <accountManager/accountManager.h>
#include <dbManager/account/accountDbManager.h>
#include <drogon/drogon.h>

std::chrono::steady_clock::time_point HealthController::startTime_ = std::chrono::steady_clock::now();

void HealthController::setStartTime(std::chrono::steady_clock::time_point startTime) {
    startTime_ = startTime;
}

void HealthController::health(const drogon::HttpRequestPtr&,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    Json::Value response(Json::objectValue);
    response["status"] = "ok";
    response["version"] = "1.1";

    const auto now = std::chrono::steady_clock::now();
    const auto uptimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - startTime_).count();
    response["uptime"] = static_cast<Json::Int64>(uptimeSeconds);

    auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(drogon::k200OK);
    callback(resp);
}

void HealthController::ready(const drogon::HttpRequestPtr&,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    Json::Value response(Json::objectValue);
    Json::Value checks(Json::objectValue);

    bool dbOk = false;
    bool providerOk = false;
    bool accountOk = false;

    try {
        dbOk = AccountDbManager::getInstance()->isTableExist();
    } catch (...) {
        dbOk = false;
    }

    try {
        providerOk = (ApiManager::getInstance().getApiByApiName("chaynsapi") != nullptr);
    } catch (...) {
        providerOk = false;
    }

    try {
        const auto accounts = AccountManager::getInstance().getAccountList();
        size_t count = 0;
        for (const auto& apiAccounts : accounts) {
            count += apiAccounts.second.size();
        }
        accountOk = count > 0;
        checks["account_count"] = static_cast<Json::UInt64>(count);
    } catch (...) {
        accountOk = false;
        checks["account_count"] = static_cast<Json::UInt64>(0);
    }

    checks["database"] = dbOk;
    checks["provider"] = providerOk;
    checks["account"] = accountOk;

    const bool ready = dbOk && providerOk && accountOk;
    response["status"] = ready ? "ready" : "not_ready";
    response["checks"] = checks;

    auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(ready ? drogon::k200OK : drogon::k503ServiceUnavailable);
    callback(resp);
}

