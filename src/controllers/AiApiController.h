#pragma once

#include <controllers/RateLimitFilter.h>
#include <drogon/HttpController.h>
#include <domain/port/IAiApiUseCase.h>

class AiApiController : public drogon::HttpController<AiApiController>
{
  public:
    // Drogon owns controller instances, so AppWiring publishes the one
    // controller-facing use case as a non-owning static binding.
    static void setUseCase(aiapi::IAiApiUseCase* useCase);

    METHOD_LIST_BEGIN
    // AI 核心 API（不添加 AdminAuthFilter，保持原有认证方式）
    ADD_METHOD_TO(AiApiController::chaynsapichat, "/chaynsapi/v1/chat/completions", drogon::Post, "RateLimitFilter");
    ADD_METHOD_TO(AiApiController::retiredNexos, "/nexosapi/v1/chat/completions", drogon::Post);
    ADD_METHOD_TO(AiApiController::chaynsapichat, "/retoolapi/v1/chat/completions", drogon::Post, "RateLimitFilter");
    ADD_METHOD_TO(AiApiController::chaynsapimodels, "/chaynsapi/v1/models", drogon::Get);
    ADD_METHOD_TO(AiApiController::retiredNexos, "/nexosapi/v1/models", drogon::Get);
    ADD_METHOD_TO(AiApiController::chaynsapimodels, "/retoolapi/v1/models", drogon::Get);
    ADD_METHOD_TO(AiApiController::retiredNexos, "/nexosapi/v1/account/quota", drogon::Get);
    // OpenAI Responses API 兼容接口（核心 API，不加管理认证）
    ADD_METHOD_TO(AiApiController::responsesCreate, "/chaynsapi/v1/responses", drogon::Post, "RateLimitFilter");
    ADD_METHOD_TO(AiApiController::retiredNexos, "/nexosapi/v1/responses", drogon::Post);
    ADD_METHOD_TO(AiApiController::responsesCreate, "/retoolapi/v1/responses", drogon::Post, "RateLimitFilter");
    ADD_METHOD_TO(AiApiController::responsesGet, "/chaynsapi/v1/responses/{1}", drogon::Get);
    ADD_METHOD_TO(AiApiController::retiredNexosWithId, "/nexosapi/v1/responses/{1}", drogon::Get);
    ADD_METHOD_TO(AiApiController::responsesGet, "/retoolapi/v1/responses/{1}", drogon::Get);
    ADD_METHOD_TO(AiApiController::responsesDelete, "/chaynsapi/v1/responses/{1}", drogon::Delete);
    ADD_METHOD_TO(AiApiController::retiredNexosWithId, "/nexosapi/v1/responses/{1}", drogon::Delete);
    ADD_METHOD_TO(AiApiController::responsesDelete, "/retoolapi/v1/responses/{1}", drogon::Delete);
    METHOD_LIST_END

    void chaynsapichat(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void chaynsapimodels(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void retiredNexos(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void retiredNexosWithId(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string ignoredId);
    // OpenAI Responses API 兼容接口
    void responsesCreate(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void responsesGet(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string responseId);
    void responsesDelete(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string responseId);

    std::string generateClientId(const drogon::HttpRequestPtr &req);
    bool isCreateNewSession(const drogon::HttpRequestPtr &req);

  private:
    static aiapi::IAiApiUseCase* useCase_;
};
