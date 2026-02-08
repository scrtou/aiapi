#include "AiApiController.h"
#include <drogon/HttpResponse.h>
#include <json/json.h>
#include <drogon/drogon.h>
#include <unistd.h>
#include <random>
#include <unordered_map>
#include <apiManager/ApiManager.h>
#include <sessionManager/core/Session.h>
#include <sessionManager/core/ClientOutputSanitizer.h>
#include <sessionManager/core/GenerationService.h>
#include <sessionManager/contracts/GenerationRequest.h>
#include <sessionManager/contracts/IResponseSink.h>
#include <sessionManager/core/SessionExecutionGate.h>
#include <sessionManager/core/Errors.h>
#include <sessionManager/core/RequestAdapters.h>
#include <sessionManager/continuity/ResponseIndex.h>
#include <utils/BackgroundTaskQueue.h>
#include "ControllerUtils.h"
#include <controllers/sinks/ChatSseSink.h>
#include <controllers/sinks/ChatJsonSink.h>
#include <controllers/sinks/ResponsesSseSink.h>
#include <controllers/sinks/ResponsesJsonSink.h>
#include <vector>
#include <ctime>
#include <optional>
#include <cstring>
#include <algorithm>
using namespace drogon;

namespace {

generation::ErrorCode toGenerationErrorCode(error::ErrorCode code)
{
    switch (code) {
        case error::ErrorCode::BadRequest:   return generation::ErrorCode::BadRequest;
        case error::ErrorCode::Unauthorized: return generation::ErrorCode::Unauthorized;
        case error::ErrorCode::Forbidden:    return generation::ErrorCode::Forbidden;
        case error::ErrorCode::NotFound:     return generation::ErrorCode::NotFound;
        case error::ErrorCode::Conflict:     return generation::ErrorCode::Conflict;
        case error::ErrorCode::RateLimited:  return generation::ErrorCode::RateLimited;
        case error::ErrorCode::Timeout:      return generation::ErrorCode::Timeout;
        case error::ErrorCode::ProviderError:return generation::ErrorCode::ProviderError;
        case error::ErrorCode::Cancelled:    return generation::ErrorCode::Cancelled;
        case error::ErrorCode::None:
        case error::ErrorCode::Internal:
        default:
            return generation::ErrorCode::Internal;
    }
}

void emitAppErrorToSink(const error::AppError& appError, IResponseSink& sink)
{
    generation::Error errorEvent;
    errorEvent.code = toGenerationErrorCode(appError.code);
    errorEvent.message = appError.message.empty() ? "Internal server error" : appError.message;
    errorEvent.detail = appError.detail;
    errorEvent.providerCode = appError.providerCode;
    sink.onEvent(errorEvent);
}

class FanoutSink final : public IResponseSink
{
public:
    FanoutSink(IResponseSink& left, IResponseSink& right)
        : left_(left), right_(right)
    {
    }

    void onEvent(const generation::GenerationEvent& event) override
    {
        left_.onEvent(event);
        right_.onEvent(event);
    }

    void onClose() override
    {
        left_.onClose();
        right_.onClose();
    }

    bool isValid() const override
    {
        return left_.isValid() && right_.isValid();
    }

    std::string getSinkType() const override
    {
        return left_.getSinkType() + "+" + right_.getSinkType();
    }

private:
    IResponseSink& left_;
    IResponseSink& right_;
};

}



void AiApiController::chaynsapichat(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[AI接口控制器] 收到聊天补全请求";
    LOG_DEBUG<<"请求头：";
    for(auto &header : req->getHeaders())
    {
        LOG_DEBUG<<header.first<<":"<<header.second;
    }
    LOG_DEBUG<<"请求体："<<req->getBody();

    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;
    auto& reqbody = *jsonPtr;

    auto& reqmessages = reqbody["messages"];
    if (reqmessages.empty()) {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "Messages array cannot be empty");
        return;
    }
    
    const bool stream = reqbody["stream"].asBool();
    LOG_INFO << "[AI接口控制器] 聊天补全 stream=" << stream;
    
    // 通过 RequestAdapters 构建 GenerationRequest
    LOG_DEBUG << "[AI接口控制器] 通过 RequestAdapters 构建 GenerationRequest";
    GenerationRequest genReq = RequestAdapters::buildGenerationRequestFromChat(req);
    

    if (genReq.provider.empty()) {
        genReq.provider = "chaynsapi";
    }
    

    if (!stream) {
        LOG_INFO << "[AI接口控制器] 执行 GenerationService::runGuarded（非流式）";
        
        HttpResponsePtr jsonResp;
        int httpStatus = 200;
        
        ChatJsonSink jsonSink(
            [&jsonResp, &httpStatus](const Json::Value& response, int status) {
                jsonResp = HttpResponse::newHttpJsonResponse(response);
                httpStatus = status;
            },
            genReq.model
        );
        
        GenerationService genService;
        auto err = genService.runGuarded(
            genReq, jsonSink,
            session::ConcurrencyPolicy::RejectConcurrent
        );
        
        if (err.has_value()) {
            Json::Value errorJson;
            errorJson["error"]["message"] = err->message;
            errorJson["error"]["type"] = err->type();
            ctl::sendJson(callback, errorJson, static_cast<HttpStatusCode>(err->httpStatus()));
            return;
        }
        
        if (jsonResp) {
            jsonResp->setStatusCode(static_cast<HttpStatusCode>(httpStatus));
            jsonResp->setContentTypeString("application/json; charset=utf-8");
            callback(jsonResp);
        } else {
            ctl::sendError(callback, k500InternalServerError, "internal_error", "Failed to generate response");
        }
        return;
    }
    

    LOG_INFO << "[AI接口控制器] 进入流式响应模式";
    LOG_DEBUG << "[AI接口控制器] previousResponseId："
              << (genReq.previousResponseId.has_value() ? *genReq.previousResponseId : "");

    auto resp = HttpResponse::newAsyncStreamResponse(
        [genReq](ResponseStreamPtr stream) mutable {
            if (!stream) {
                LOG_WARN << "[AI接口控制器] 流对象为空，终止处理";
                return;
            }

            auto sharedStream = std::shared_ptr<ResponseStream>(stream.release());
            BackgroundTaskQueue::instance().enqueue("chat_stream_generation", [sharedStream, genReq]() mutable {
                ChatSseSink sseSink(
                    [sharedStream](const std::string& chunk) {
                        return sharedStream && sharedStream->send(chunk);
                    },
                    [sharedStream]() {
                        if (sharedStream) {
                            sharedStream->close();
                        }
                    },
                    genReq.model
                );

                GenerationService genService;
                auto runErr = genService.runGuarded(
                    genReq,
                    sseSink,
                    session::ConcurrencyPolicy::RejectConcurrent
                );

                if (runErr.has_value() && sseSink.isValid()) {
                    emitAppErrorToSink(*runErr, sseSink);
                    sseSink.onClose();
                }
            });
        },
        true
    );

    resp->setContentTypeString("text/event-stream; charset=utf-8");
    resp->addHeader("Cache-Control", "no-cache");
    resp->addHeader("Connection", "keep-alive");
    resp->addHeader("X-Accel-Buffering", "no");
    resp->addHeader("Keep-Alive", "timeout=60");
    callback(resp);
    LOG_INFO << "[AI接口控制器] 聊天流响应已开始发送";
}

void AiApiController::chaynsapimodels(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[AI接口控制器] 获取模型列表";
    Json::Value response= ApiManager::getInstance().getApiByApiName("chaynsapi")->getModels();
    ctl::sendJson(callback, response);
}

// ===========================================================

// ===========================================================

void AiApiController::responsesCreate(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[AI接口控制器] 收到 Responses 创建请求";
    
    LOG_DEBUG << "请求头：";
    for(auto &header : req->getHeaders()) {
        LOG_DEBUG << header.first << ":" << header.second;
    }
    LOG_DEBUG << "请求体：" << req->getBody();

    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr, "invalid_json", "Invalid JSON in request body")) return;
    
    auto& reqBody = *jsonPtr;
    const bool stream = reqBody.get("stream", false).asBool();

    // 通过 RequestAdapters 构建 GenerationRequest
    LOG_DEBUG << "[AI接口控制器] 通过 RequestAdapters 构建 GenerationRequest";
    GenerationRequest genReq = RequestAdapters::buildGenerationRequestFromResponses(req);


    if (genReq.currentInput.empty() && genReq.messages.empty()) {
        ctl::sendError(callback, k400BadRequest, "missing_input", "Input cannot be empty");
        return;
    }


    if (!stream) {
        LOG_INFO << "[AI接口控制器] 执行 GenerationService::runGuarded（非流式 Responses）";

        HttpResponsePtr jsonResp;
        int httpStatus = 200;

        ResponsesJsonSink jsonSink(
            [&jsonResp, &httpStatus](const Json::Value& builtResponse, int status) {
                if (status == 200 && !builtResponse.isMember("error") &&
                    builtResponse.isMember("id") && builtResponse["id"].isString()) {
                    ResponseIndex::instance().storeResponse(builtResponse["id"].asString(), builtResponse);
                }

                jsonResp = HttpResponse::newHttpJsonResponse(builtResponse);
                httpStatus = status;
            },
            genReq.model,
            static_cast<int>(genReq.currentInput.length() / 4)
        );

        GenerationService genService;
        auto gateErr = genService.runGuarded(
            genReq, jsonSink,
            session::ConcurrencyPolicy::RejectConcurrent
        );

        if (gateErr.has_value()) {
            Json::Value errorJson;
            errorJson["error"]["message"] = gateErr->message;
            errorJson["error"]["type"] = gateErr->type();
            errorJson["error"]["code"] = "concurrent_request";
            ctl::sendJson(callback, errorJson, static_cast<HttpStatusCode>(gateErr->httpStatus()));
            return;
        }

        if (jsonResp) {
            jsonResp->setStatusCode(static_cast<HttpStatusCode>(httpStatus));
            jsonResp->setContentTypeString("application/json; charset=utf-8");
            callback(jsonResp);
        } else {
            ctl::sendError(callback, k500InternalServerError, "internal_error", "Failed to generate response");
        }

        return;
    }

    // 流式 Responses：通过 ResponsesSseSink 输出事件流
    LOG_INFO << "[AI接口控制器] Responses 进入流式响应模式";

    auto resp = HttpResponse::newAsyncStreamResponse(
        [genReq](ResponseStreamPtr stream) mutable {
            if (!stream) {
                LOG_WARN << "[AI接口控制器] Responses 流对象为空，终止处理";
                return;
            }

            auto sharedStream = std::shared_ptr<ResponseStream>(stream.release());
            BackgroundTaskQueue::instance().enqueue("responses_stream_generation", [sharedStream, genReq]() mutable {
                CollectorSink collector;
                ResponsesSseSink sseSink(
                    [sharedStream](const std::string& chunk) {
                        return sharedStream && sharedStream->send(chunk);
                    },
                    [sharedStream]() {
                        if (sharedStream) {
                            sharedStream->close();
                        }
                    },
                    genReq.model
                );

                FanoutSink fanout(sseSink, collector);

                GenerationService genService;
                auto runErr = genService.runGuarded(
                    genReq,
                    fanout,
                    session::ConcurrencyPolicy::RejectConcurrent
                );

                if (runErr.has_value()) {
                    if (sseSink.isValid()) {
                        emitAppErrorToSink(*runErr, sseSink);
                        sseSink.onClose();
                    }
                    return;
                }

                Json::Value builtResponse;
                int builtStatus = 200;
                ResponsesJsonSink jsonBuilder(
                    [&builtResponse, &builtStatus](const Json::Value& resp, int status) {
                        builtResponse = resp;
                        builtStatus = status;
                    },
                    genReq.model,
                    static_cast<int>(genReq.currentInput.length() / 4)
                );
                for (const auto& ev : collector.getEvents()) {
                    jsonBuilder.onEvent(ev);
                }
                jsonBuilder.onClose();

                if (builtStatus == 200 && !builtResponse.isMember("error") &&
                    builtResponse.isMember("id") && builtResponse["id"].isString()) {
                    ResponseIndex::instance().storeResponse(builtResponse["id"].asString(), builtResponse);
                }
            });
        },
        true
    );

    resp->setContentTypeString("text/event-stream; charset=utf-8");
    resp->addHeader("Cache-Control", "no-cache");
    resp->addHeader("Connection", "keep-alive");
    resp->addHeader("X-Accel-Buffering", "no");
    resp->addHeader("Keep-Alive", "timeout=60");
    callback(resp);
}

void AiApiController::responsesGet(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback, std::string responseId)
{
    LOG_INFO << "[AI接口控制器] ResponsesGet - ID：" << responseId;

    Json::Value stored;
    if (!ResponseIndex::instance().tryGetResponse(responseId, stored)) {
        ctl::sendError(callback, k404NotFound, "invalid_request_error", "Response not found", "response_not_found");
        return;
    }

    ctl::sendJson(callback, stored);
}

void AiApiController::responsesDelete(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback, std::string responseId)
{
    LOG_INFO << "[AI接口控制器] Responses删除 - ID：" << responseId;

    if (!ResponseIndex::instance().erase(responseId)) {
        ctl::sendError(callback, k404NotFound, "invalid_request_error", "Response not found", "response_not_found");
        return;
    }
    
    Json::Value response;
    response["id"] = responseId;
    response["object"] = "response";
    response["deleted"] = true;
    
    ctl::sendJson(callback, response);
    
    LOG_INFO << "[AI接口控制器] Responses删除 完成，ID：" << responseId;
}



std::string AiApiController::generateClientId(const HttpRequestPtr &req)
{
    std::string clientIp = req->getPeerAddr().toIp();
    std::string userAgent = req->getHeader("User-Agent");
    return clientIp + "_" + std::to_string(std::hash<std::string>{}(userAgent));
}

bool AiApiController::isCreateNewSession(const HttpRequestPtr &req)
{
    auto jsonPtr = req->getJsonObject();
    if (jsonPtr && jsonPtr->isMember("new_session")) {
        return (*jsonPtr)["new_session"].asBool();
    }
    return false;
}
