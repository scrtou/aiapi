#pragma once

#include <drogon/drogon.h>
#include <chrono>
#include <ctime>
#include <string>

#include <domain/port/IBackgroundExecutor.h>

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
inline drogon::HttpResponsePtr makeError(
    drogon::HttpStatusCode status,
    const std::string& type,
    const std::string& message,
    const std::string& code = std::string())
{
    Json::Value error;
    error["error"]["message"] = message;
    error["error"]["type"] = type;
    if (!code.empty()) {
        error["error"]["code"] = code;
    }
    auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
    resp->setStatusCode(status);
    return resp;
}

/**
 * @brief 把 executor 提交失败翻译成 HTTP 响应；Accepted 返回 nullptr。
 *
 * 为什么必须区分两类拒绝：QueueFull 是**瞬时**背压，同一进程稍后会恢复，
 * 因此带 `Retry-After` 邀请客户端重试；ShuttingDown/Stopped 是**终态**，
 * 本进程再也不会接受任务，给 Retry-After 只会诱导客户端向一个正在消失的
 * 实例重试，把本该由负载均衡器摘除的流量继续打回来。两者都用 503，靠
 * `error.code` 区分，让调用方无需了解队列内部状态机。
 *
 * 返回 nullptr 而非抛异常：调用点大多在已构造好响应的路径上，
 * 用「非空即拒绝」的哨兵可以让迁移后的代码保持单一出口。
 */
inline drogon::HttpResponsePtr makeEnqueueRejection(TaskSubmitResult result)
{
    switch (result) {
        case TaskSubmitResult::Accepted:
            return nullptr;

        case TaskSubmitResult::QueueFull: {
            auto resp = makeError(
                drogon::k503ServiceUnavailable, "service_unavailable",
                "Server is busy, please retry later", "queue_full");
            resp->addHeader("Retry-After", "1");
            return resp;
        }

        case TaskSubmitResult::ShuttingDown:
        case TaskSubmitResult::Stopped:
            // 刻意不加 Retry-After：见上方说明。
            return makeError(
                drogon::k503ServiceUnavailable, "service_unavailable",
                "Server is shutting down", "shutting_down");
    }
    return makeError(drogon::k500InternalServerError, "internal_error",
                     "Unknown background task submit result");
}

inline void respondInLoop(
    const std::shared_ptr<std::function<void(const drogon::HttpResponsePtr&)>>& cb,
    const drogon::HttpResponsePtr& resp)
{
    if (!cb || !(*cb)) {
        return;
    }
    drogon::app().getLoop()->queueInLoop([cb, resp]() {
        (*cb)(resp);
    });
}

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
