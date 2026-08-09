#include <controllers/RetiredProviderTombstone.h>

#include <drogon/drogon.h>
#include <sessionManager/core/RetiredProviderTelemetry.h>

namespace retired_provider {
namespace {

std::string requestIdOf(const drogon::HttpRequestPtr& req)
{
    if (!req) return {};
    auto value = req->getHeader("x-request-id");
    if (value.empty()) value = req->getHeader("X-Request-ID");
    return value;
}

Json::Value replacementProviders()
{
    Json::Value providers(Json::arrayValue);
    providers.append("chaynsapi");
    providers.append("retoolapi");
    return providers;
}

}  // namespace

TombstoneMetric makeNexosTombstoneMetric(const drogon::HttpRequestPtr& req)
{
    TombstoneMetric metric;
    metric.requestId = requestIdOf(req);
    metric.path = req ? req->path() : "";
    metric.method = req ? req->methodString() : "";
    metric.detail["retirement_id"] = kNexosRetirementId;
    metric.detail["legacy_route_family"] = "/nexosapi/v1/*";
    metric.detail["replacement_providers"] = replacementProviders();
    metric.detail["path"] = metric.path;
    metric.detail["method"] = metric.method;
    return metric;
}

void recordNexosTombstoneMetric(const drogon::HttpRequestPtr& req)
{
    const auto metric = makeNexosTombstoneMetric(req);
    observability::recordRetiredProviderRoute(
        metric.requestId, kNexosRetirementStatus, metric.detail);
}

void respondNexosTombstone(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    recordNexosTombstoneMetric(req);

    Json::Value body(Json::objectValue);
    body["error"]["type"] = "provider_retired";
    body["error"]["code"] = "provider_retired";
    body["error"]["message"] =
        "The nexosapi provider has been retired; use /chaynsapi/v1/* or /retoolapi/v1/* instead.";
    body["retirement_id"] = kNexosRetirementId;
    body["replacement_providers"] = replacementProviders();

    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(kNexosRetirementStatus));
    response->addHeader("X-AIAPI-Retirement-Id", kNexosRetirementId);
    response->addHeader("Cache-Control", "no-store");
    callback(response);
}

}  // namespace retired_provider
