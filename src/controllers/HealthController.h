#pragma once

#include <drogon/HttpController.h>

class HealthController : public drogon::HttpController<HealthController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthController::health, "/health", drogon::Get);
    ADD_METHOD_TO(HealthController::ready, "/ready", drogon::Get);
    METHOD_LIST_END

    static void setStartTime(std::chrono::steady_clock::time_point startTime);

    void health(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void ready(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    static std::chrono::steady_clock::time_point startTime_;
};

