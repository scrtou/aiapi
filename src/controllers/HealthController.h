#pragma once

#include <drogon/HttpController.h>
#include <domain/port/IAccountStore.h>
#include <memory>

class HealthController : public drogon::HttpController<HealthController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthController::health, "/health", drogon::Get);
    ADD_METHOD_TO(HealthController::ready, "/ready", drogon::Get);
    METHOD_LIST_END

    static void setStartTime(std::chrono::steady_clock::time_point startTime);

    // /ready 的数据库探针。刻意复用既有的 IAccountStore 端口而非新造 IHealthProbe：
    // 本控制器只需要 isTableExist()，该方法已是 IAccountStore 的纯虚成员，
    // 且 main.cc 注入给 AccountManager 的正是同一个实例。
    // 未注入时 /ready 的 database 项判为 false（降级而非崩溃），由启动接线门禁守住。
    static void setDbProbe(std::shared_ptr<IAccountStore> probe);

    void health(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void ready(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    static std::chrono::steady_clock::time_point startTime_;
    static std::shared_ptr<IAccountStore> dbProbe_;
};

