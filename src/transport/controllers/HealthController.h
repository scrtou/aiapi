#pragma once

#include <drogon/HttpController.h>
#include <domain/port/IHealthUseCase.h>

class HealthController : public drogon::HttpController<HealthController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthController::health, "/health", drogon::Get);
    ADD_METHOD_TO(HealthController::ready, "/ready", drogon::Get);
    METHOD_LIST_END

    // Drogon creates controllers itself, so composition-root injection is
    // published through this static binding.  The controller knows only the
    // controller-facing health workflow, not stores/catalogs/providers.
    static void setUseCase(IHealthUseCase* health);

    void health(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void ready(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    static IHealthUseCase* useCase_;
};
