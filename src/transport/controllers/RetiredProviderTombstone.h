#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <functional>
#include <string>

namespace retired_provider {

inline constexpr const char* kNexosRetirementId = "retire-nexos-openai-v1";
inline constexpr int kNexosRetirementStatus = 410;

struct TombstoneMetric
{
    std::string requestId;
    std::string path;
    std::string method;
    Json::Value detail;
};

/// Pure observation assembly used by the production handler and offline tests.
TombstoneMetric makeNexosTombstoneMetric(const drogon::HttpRequestPtr& req);

/// Record one legacy-route call without putting the retired provider back into
/// the active provider/request metric dimension.
void recordNexosTombstoneMetric(const drogon::HttpRequestPtr& req);

/// Build the stable 410 response shared by every /nexosapi/v1/* route.
void respondNexosTombstone(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

}  // namespace retired_provider
