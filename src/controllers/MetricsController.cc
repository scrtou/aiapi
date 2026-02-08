#include "MetricsController.h"
#include "ControllerUtils.h"
#include "ErrorStatsDbManager.h"
#include "StatusDbManager.h"

using namespace drogon;

// ========== 请求 / 错误时序 ==========

void MetricsController::getRequestsSeries(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[MetricsCtrl] 获取请求时序统计";

    std::string from = req->getParameter("from");
    std::string to   = req->getParameter("to");
    ctl::defaultTimeRange(from, to);

    metrics::QueryParams params;
    params.from = from;
    params.to   = to;

    auto dbManager = metrics::ErrorStatsDbManager::getInstance();
    auto series = dbManager->queryRequestSeries(params);

    Json::Value response(Json::objectValue);
    response["from"] = from;
    response["to"]   = to;

    Json::Value data(Json::arrayValue);
    for (const auto& bucket : series) {
        Json::Value item;
        item["bucket_start"] = bucket.bucketStart;
        item["count"]        = static_cast<Json::Int64>(bucket.count);
        data.append(item);
    }
    response["data"] = data;

    ctl::sendJson(callback, response);
}

void MetricsController::getErrorsSeries(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[MetricsCtrl] 获取错误时序统计";

    std::string from       = req->getParameter("from");
    std::string to         = req->getParameter("to");
    std::string severity   = req->getParameter("severity");
    std::string domain     = req->getParameter("domain");
    std::string type       = req->getParameter("type");
    std::string provider   = req->getParameter("provider");
    std::string model      = req->getParameter("model");
    std::string clientType = req->getParameter("client_type");

    ctl::defaultTimeRange(from, to);

    metrics::QueryParams params;
    params.from       = from;
    params.to         = to;
    params.severity   = severity;
    params.domain     = domain;
    params.type       = type;
    params.provider   = provider;
    params.model      = model;
    params.clientType = clientType;

    auto dbManager = metrics::ErrorStatsDbManager::getInstance();
    auto series = dbManager->queryErrorSeries(params);

    Json::Value response(Json::objectValue);
    response["from"] = from;
    response["to"]   = to;
    if (!severity.empty())   response["severity"]    = severity;
    if (!domain.empty())     response["domain"]      = domain;
    if (!type.empty())       response["type"]        = type;
    if (!provider.empty())   response["provider"]    = provider;
    if (!model.empty())      response["model"]       = model;
    if (!clientType.empty()) response["client_type"] = clientType;

    Json::Value data(Json::arrayValue);
    for (const auto& bucket : series) {
        Json::Value item;
        item["bucket_start"] = bucket.bucketStart;
        item["count"]        = static_cast<Json::Int64>(bucket.count);
        data.append(item);
    }
    response["data"] = data;

    ctl::sendJson(callback, response);
}

// ========== 错误事件 ==========

void MetricsController::getErrorsEvents(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[MetricsCtrl] 获取错误事件列表";

    std::string from = req->getParameter("from");
    std::string to   = req->getParameter("to");
    int limit  = 100;
    int offset = 0;

    std::string limitStr  = req->getParameter("limit");
    std::string offsetStr = req->getParameter("offset");
    if (!limitStr.empty())  { try { limit  = std::stoi(limitStr);  } catch (...) {} }
    if (!offsetStr.empty()) { try { offset = std::stoi(offsetStr); } catch (...) {} }

    if (limit > 1000) limit = 1000;
    if (limit < 1)    limit = 1;
    if (offset < 0)   offset = 0;

    ctl::defaultTimeRange(from, to);

    metrics::QueryParams params;
    params.from = from;
    params.to   = to;

    auto dbManager = metrics::ErrorStatsDbManager::getInstance();
    auto events = dbManager->queryEvents(params, limit, offset);

    Json::Value response(Json::objectValue);
    response["from"]   = from;
    response["to"]     = to;
    response["limit"]  = limit;
    response["offset"] = offset;

    Json::Value data(Json::arrayValue);
    for (const auto& ev : events) {
        Json::Value item;
        item["id"]          = static_cast<Json::Int64>(ev.id);
        item["ts"]          = ev.ts;
        item["severity"]    = ev.severity;
        item["domain"]      = ev.domain;
        item["type"]        = ev.type;
        item["provider"]    = ev.provider;
        item["model"]       = ev.model;
        item["client_type"] = ev.clientType;
        item["api_kind"]    = ev.apiKind;
        item["stream"]      = ev.stream;
        item["http_status"] = ev.httpStatus;
        item["request_id"]  = ev.requestId;
        item["message"]     = ev.message;
        data.append(item);
    }
    response["data"]  = data;
    response["count"] = static_cast<Json::UInt64>(events.size());

    ctl::sendJson(callback, response);
}

void MetricsController::getErrorsEventById(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback, int64_t id)
{
    LOG_INFO << "[MetricsCtrl] 获取错误事件详情 - ID：" << id;

    auto dbManager = metrics::ErrorStatsDbManager::getInstance();
    auto eventOpt = dbManager->queryEventById(id);

    if (!eventOpt.has_value()) {
        ctl::sendError(callback, HttpStatusCode::k404NotFound,
                       "invalid_request_error", "Event not found", "event_not_found");
        return;
    }

    const auto& ev = eventOpt.value();

    Json::Value response;
    response["id"]          = static_cast<Json::Int64>(ev.id);
    response["ts"]          = ev.ts;
    response["severity"]    = ev.severity;
    response["domain"]      = ev.domain;
    response["type"]        = ev.type;
    response["provider"]    = ev.provider;
    response["model"]       = ev.model;
    response["client_type"] = ev.clientType;
    response["api_kind"]    = ev.apiKind;
    response["stream"]      = ev.stream;
    response["http_status"] = ev.httpStatus;
    response["request_id"]  = ev.requestId;
    response["message"]     = ev.message;
    response["detail_json"] = ev.detailJson;
    response["raw_snippet"] = ev.rawSnippet;

    ctl::sendJson(callback, response);
}

// ========== 服务状态监控 ==========

void MetricsController::getStatusSummary(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[MetricsCtrl] 获取服务状态概览";

    std::string from = req->getParameter("from");
    std::string to   = req->getParameter("to");

    metrics::StatusQueryParams params;
    params.from = from;
    params.to   = to;

    auto statusManager = metrics::StatusDbManager::getInstance();
    auto summary = statusManager->getStatusSummary(params);

    Json::Value response(Json::objectValue);
    response["total_requests"]   = static_cast<Json::Int64>(summary.totalRequests);
    response["total_errors"]     = static_cast<Json::Int64>(summary.totalErrors);
    response["error_rate"]       = summary.errorRate;
    response["channel_count"]    = summary.channelCount;
    response["model_count"]      = summary.modelCount;
    response["healthy_channels"] = summary.healthyChannels;
    response["degraded_channels"]= summary.degradedChannels;
    response["down_channels"]    = summary.downChannels;
    response["overall_status"]   = metrics::StatusDbManager::statusToString(summary.overallStatus);

    Json::Value buckets(Json::arrayValue);
    for (const auto& bucket : summary.buckets) {
        Json::Value item;
        item["bucket_start"]  = bucket.bucketStart;
        item["request_count"] = static_cast<Json::Int64>(bucket.requestCount);
        item["error_count"]   = static_cast<Json::Int64>(bucket.errorCount);
        item["error_rate"]    = bucket.errorRate;
        buckets.append(item);
    }
    response["buckets"] = buckets;

    ctl::sendJson(callback, response);
}

void MetricsController::getStatusChannels(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[MetricsCtrl] 获取渠道状态列表";

    std::string from     = req->getParameter("from");
    std::string to       = req->getParameter("to");
    std::string provider = req->getParameter("provider");

    metrics::StatusQueryParams params;
    params.from     = from;
    params.to       = to;
    params.provider = provider;

    auto statusManager = metrics::StatusDbManager::getInstance();
    auto channels = statusManager->getChannelStatusList(params);

    Json::Value response(Json::objectValue);
    Json::Value data(Json::arrayValue);

    for (const auto& ch : channels) {
        Json::Value item;
        item["channel_id"]       = ch.channelId;
        item["channel_name"]     = ch.channelName;
        item["total_requests"]   = static_cast<Json::Int64>(ch.totalRequests);
        item["total_errors"]     = static_cast<Json::Int64>(ch.totalErrors);
        item["error_rate"]       = ch.errorRate;
        item["status"]           = metrics::StatusDbManager::statusToString(ch.status);
        item["last_request_time"]= ch.lastRequestTime;

        Json::Value buckets(Json::arrayValue);
        for (const auto& bucket : ch.buckets) {
            Json::Value b;
            b["bucket_start"]  = bucket.bucketStart;
            b["request_count"] = static_cast<Json::Int64>(bucket.requestCount);
            b["error_count"]   = static_cast<Json::Int64>(bucket.errorCount);
            b["error_rate"]    = bucket.errorRate;
            buckets.append(b);
        }
        item["buckets"] = buckets;

        data.append(item);
    }

    response["data"]  = data;
    response["count"] = static_cast<Json::UInt64>(channels.size());

    ctl::sendJson(callback, response);
}

void MetricsController::getStatusModels(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[MetricsCtrl] 获取模型状态列表";

    std::string from     = req->getParameter("from");
    std::string to       = req->getParameter("to");
    std::string provider = req->getParameter("provider");
    std::string model    = req->getParameter("model");

    metrics::StatusQueryParams params;
    params.from     = from;
    params.to       = to;
    params.provider = provider;
    params.model    = model;

    auto statusManager = metrics::StatusDbManager::getInstance();
    auto models = statusManager->getModelStatusList(params);

    Json::Value response(Json::objectValue);
    Json::Value data(Json::arrayValue);

    for (const auto& m : models) {
        Json::Value item;
        item["model"]            = m.model;
        item["provider"]         = m.provider;
        item["total_requests"]   = static_cast<Json::Int64>(m.totalRequests);
        item["total_errors"]     = static_cast<Json::Int64>(m.totalErrors);
        item["error_rate"]       = m.errorRate;
        item["status"]           = metrics::StatusDbManager::statusToString(m.status);
        item["last_request_time"]= m.lastRequestTime;

        Json::Value buckets(Json::arrayValue);
        for (const auto& bucket : m.buckets) {
            Json::Value b;
            b["bucket_start"]  = bucket.bucketStart;
            b["request_count"] = static_cast<Json::Int64>(bucket.requestCount);
            b["error_count"]   = static_cast<Json::Int64>(bucket.errorCount);
            b["error_rate"]    = bucket.errorRate;
            buckets.append(b);
        }
        item["buckets"] = buckets;

        data.append(item);
    }

    response["data"]  = data;
    response["count"] = static_cast<Json::UInt64>(models.size());

    ctl::sendJson(callback, response);
}
