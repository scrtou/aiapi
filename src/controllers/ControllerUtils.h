#pragma once

#include <drogon/drogon.h>
#include <chrono>
#include <ctime>
#include <string>

/**
 * @brief Controller 层通用工具函数
 *
 * 用于统一 JSON 成功/失败响应构建，并减少各控制器中的重复代码。
 *
 * 用法：
 *   ctl::sendError(callback, k400BadRequest, "invalid_request_error", "missing field: model");
 *   ctl::sendJson(callback, responseJson);
 *   ctl::sendJson(callback, responseJson, k201Created);
 */
namespace ctl {

// 构建标准错误 JSON 响应并回调
inline void sendError(
    std::function<void(const drogon::HttpResponsePtr&)>& callback,
    drogon::HttpStatusCode status,
    const std::string& type,
    const std::string& message)
{
    Json::Value error;
    error["error"]["message"] = message;
    error["error"]["type"] = type;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
    resp->setStatusCode(status);
    callback(resp);
}

// 构建带额外 code 字段的错误 JSON 响应
inline void sendError(
    std::function<void(const drogon::HttpResponsePtr&)>& callback,
    drogon::HttpStatusCode status,
    const std::string& type,
    const std::string& message,
    const std::string& code)
{
    Json::Value error;
    error["error"]["message"] = message;
    error["error"]["type"] = type;
    error["error"]["code"] = code;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
    resp->setStatusCode(status);
    callback(resp);
}

// 构建成功 JSON 响应并回调（默认 200 OK）
inline void sendJson(
    std::function<void(const drogon::HttpResponsePtr&)>& callback,
    const Json::Value& data,
    drogon::HttpStatusCode status = drogon::k200OK)
{
    auto resp = drogon::HttpResponse::newHttpJsonResponse(data);
    resp->setStatusCode(status);
    callback(resp);
}

// 构建成功 JSON 响应并强制 UTF-8 Content-Type
inline void sendJsonUtf8(
    std::function<void(const drogon::HttpResponsePtr&)>& callback,
    const Json::Value& data,
    drogon::HttpStatusCode status = drogon::k200OK)
{
    auto resp = drogon::HttpResponse::newHttpJsonResponse(data);
    resp->setStatusCode(status);
    resp->setContentTypeString("application/json; charset=utf-8");
    callback(resp);
}

/**
 * @brief 解析请求体 JSON；失败时自动返回标准错误响应。
 *
 * @return true 解析成功；false 解析失败（已回调错误响应）
 */
inline bool parseJsonOrError(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>& callback,
    std::shared_ptr<Json::Value>& outJson,
    const std::string& errType = "invalid_request_error",
    const std::string& errMessage = "Invalid JSON in request body",
    drogon::HttpStatusCode status = drogon::k400BadRequest)
{
    outJson = req->getJsonObject();
    if (!outJson) {
        ctl::sendError(callback, status, errType, errMessage);
        return false;
    }
    return true;
}

/**
 * @brief 为 from/to 提供默认 UTC 时间范围。
 *
 * 当任一参数为空时，自动补齐为“当前时间往前 hours 小时”。
 * 时间格式：`%Y-%m-%d %H:%M:%S`。
 */
inline void defaultTimeRange(std::string& from, std::string& to, int hours = 24)
{
    if (!from.empty() && !to.empty()) return;

    auto now = std::chrono::system_clock::now();
    auto start = now - std::chrono::hours(hours);

    auto formatUtc = [](std::chrono::system_clock::time_point tp) -> std::string {
        auto tt = std::chrono::system_clock::to_time_t(tp);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::gmtime(&tt));
        return std::string(buf);
    };

    if (from.empty()) from = formatUtc(start);
    if (to.empty()) to = formatUtc(now);
}

}  // namespace ctl
