#pragma once

#include "RateLimitFilter.h"
#include <drogon/HttpController.h>

class AiApiController : public drogon::HttpController<AiApiController>
{
  public:
    METHOD_LIST_BEGIN
    // AI 核心 API（不添加 AdminAuthFilter，保持原有认证方式）
    ADD_METHOD_TO(AiApiController::chaynsapichat, "/chaynsapi/v1/chat/completions", drogon::Post, "RateLimitFilter");
    ADD_METHOD_TO(AiApiController::chaynsapimodels, "/chaynsapi/v1/models", drogon::Get);
    // OpenAI Responses API 兼容接口（核心 API，不加管理认证）
    ADD_METHOD_TO(AiApiController::responsesCreate, "/chaynsapi/v1/responses", drogon::Post, "RateLimitFilter");
    ADD_METHOD_TO(AiApiController::responsesGet, "/chaynsapi/v1/responses/{1}", drogon::Get);
    ADD_METHOD_TO(AiApiController::responsesDelete, "/chaynsapi/v1/responses/{1}", drogon::Delete);
    METHOD_LIST_END


    void chaynsapichat(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void chaynsapimodels(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    // OpenAI Responses API 兼容接口
    void responsesCreate(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void responsesGet(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string responseId);
    void responsesDelete(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string responseId);


    std::string generateClientId(const drogon::HttpRequestPtr &req);
    bool isCreateNewSession(const drogon::HttpRequestPtr &req);
};
