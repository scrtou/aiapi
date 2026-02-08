#pragma once

#include "AdminAuthFilter.h"
#include <drogon/HttpController.h>

/**
 * @brief 日志查看 Controller
 *
 * 端点:
 *   GET /aichat/logs/list   – 列出日志文件
 *   GET /aichat/logs/tail   – 读取日志尾部
 */
class LogController : public drogon::HttpController<LogController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(LogController::logsList,  "/aichat/logs/list", drogon::Get, "AdminAuthFilter");
    ADD_METHOD_TO(LogController::logsTail,  "/aichat/logs/tail", drogon::Get, "AdminAuthFilter");
    METHOD_LIST_END

    void logsList(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void logsTail(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};
