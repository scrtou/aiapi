#pragma once

#include "AdminAuthFilter.h"
#include <drogon/HttpController.h>

/**
 * @brief 错误统计 & 服务状态监控 Controller
 *
 * 端点:
 *   GET /aichat/metrics/requests/series       – 请求时序统计
 *   GET /aichat/metrics/errors/series         – 错误时序统计
 *   GET /aichat/metrics/errors/events         – 错误事件列表
 *   GET /aichat/metrics/errors/events/{id}    – 错误事件详情
 *   GET /aichat/metrics/status/summary        – 服务状态概览
 *   GET /aichat/metrics/status/channels       – 渠道状态列表
 *   GET /aichat/metrics/status/models         – 模型状态列表
 */
class MetricsController : public drogon::HttpController<MetricsController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(MetricsController::getRequestsSeries,   "/aichat/metrics/requests/series",     drogon::Get, "AdminAuthFilter");
    ADD_METHOD_TO(MetricsController::getErrorsSeries,     "/aichat/metrics/errors/series",       drogon::Get, "AdminAuthFilter");
    ADD_METHOD_TO(MetricsController::getErrorsEvents,     "/aichat/metrics/errors/events",       drogon::Get, "AdminAuthFilter");
    ADD_METHOD_TO(MetricsController::getErrorsEventById,  "/aichat/metrics/errors/events/{id}",  drogon::Get, "AdminAuthFilter");
    ADD_METHOD_TO(MetricsController::getStatusSummary,    "/aichat/metrics/status/summary",      drogon::Get, "AdminAuthFilter");
    ADD_METHOD_TO(MetricsController::getStatusChannels,   "/aichat/metrics/status/channels",     drogon::Get, "AdminAuthFilter");
    ADD_METHOD_TO(MetricsController::getStatusModels,     "/aichat/metrics/status/models",       drogon::Get, "AdminAuthFilter");
    METHOD_LIST_END

    void getRequestsSeries(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void getErrorsSeries(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void getErrorsEvents(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void getErrorsEventById(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, int64_t id);
    void getStatusSummary(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void getStatusChannels(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void getStatusModels(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};
