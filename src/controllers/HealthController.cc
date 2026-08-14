#include <controllers/HealthController.h>
#include <drogon/drogon.h>

IHealthUseCase* HealthController::useCase_ = nullptr;

void HealthController::setUseCase(IHealthUseCase* health)
{
    // Composition-root only; called before app().run() and cleared before
    // teardown of the context-owned collaborators.
    useCase_ = health;
}

void HealthController::health(const drogon::HttpRequestPtr&,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    Json::Value response(Json::objectValue);
    response["status"] = "ok";
    response["version"] = "1.1";
    response["uptime"] = static_cast<Json::UInt64>(
        useCase_ ? useCase_->uptimeSeconds() : 0);

    auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(drogon::k200OK);
    callback(resp);
}

void HealthController::ready(const drogon::HttpRequestPtr&,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const HealthReadiness checksFromUseCase = useCase_
        ? useCase_->readiness() : HealthReadiness{};

    Json::Value response(Json::objectValue);
    Json::Value checks(Json::objectValue);
    checks["database"] = checksFromUseCase.database;
    checks["provider"] = checksFromUseCase.provider;
    checks["account"] = checksFromUseCase.account;
    checks["account_count"] = static_cast<Json::UInt64>(checksFromUseCase.accountCount);

    const bool ready = checksFromUseCase.ready();
    response["status"] = ready ? "ready" : "not_ready";
    response["checks"] = checks;

    auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(ready ? drogon::k200OK : drogon::k503ServiceUnavailable);
    callback(resp);
}
