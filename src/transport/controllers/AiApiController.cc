#include <transport/controllers/AiApiController.h>

#include <transport/controllers/ControllerUtils.h>
#include <transport/controllers/RetiredProviderTombstone.h>
#include <transport/controllers/codecs/ProviderModelCatalogJsonCodec.h>
#include <application/generation/contracts/IResponseSink.h>
#include <transport/controllers/sinks/IoLoopResponseStream.h>

#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

using namespace drogon;

namespace {

struct JsonResponseState {
    HttpResponsePtr response;
    int status = 200;
};

std::string inferProviderFromPath(const HttpRequestPtr& req)
{
    const auto path = req ? req->path() : "";
    if (path.rfind("/retoolapi/", 0) == 0) {
        return "retoolapi";
    }
    return "chaynsapi";
}

void logSafeRequestMetadata(const HttpRequestPtr& req, const char* endpoint)
{
    if (!req) {
        LOG_INFO << "[AI] : endpoint=" << endpoint
                  << ", requestPresent=false";
        return;
    }

    const auto& body = req->getBody();
    const auto contentType = req->getHeader("content-type");
    const auto authorization = req->getHeader("authorization");
    const auto cookie = req->getHeader("cookie");
    const char* contentTypeKind = contentType.empty() ? "absent" :
        (contentType.find("application/json") != std::string::npos ? "json" : "other");
    LOG_INFO << "[AI] : endpoint=" << endpoint
              << ", method=" << req->methodString()
              << ", path=" << req->path()
              << ", bodyPresent=" << (!body.empty())
              << ", bodySize=" << body.size()
              << ", contentTypePresent=" << (!contentType.empty())
              << ", contentTypeKind=" << contentTypeKind
              << ", authorizationPresent=" << (!authorization.empty())
              << ", cookiePresent=" << (!cookie.empty());
}

aiapi::RequestHeaders requestHeaders(const HttpRequestPtr& req)
{
    aiapi::RequestHeaders headers;
    if (!req) {
        return headers;
    }
    headers.requestId = req->getHeader("x-request-id");
    headers.correlationId = req->getHeader("x-correlation-id");
    headers.userAgent = req->getHeader("user-agent");
    headers.originator = req->getHeader("originator");
    headers.codexWindowId = req->getHeader("x-codex-window-id");
    headers.threadId = req->getHeader("thread-id");
    headers.sessionId = req->getHeader("session-id");
    headers.sessionIdUnderscore = req->getHeader("session_id");
    headers.conversationId = req->getHeader("conversation-id");
    headers.conversationIdUnderscore = req->getHeader("conversation_id");
    headers.authorization = req->getHeader("authorization");
    if (headers.authorization.empty()) {
        headers.authorization = req->getHeader("Authorization");
    }
    return headers;
}

aiapi::GenerationInput generationInput(const HttpRequestPtr& req,
                                       const Json::Value& body)
{
    aiapi::GenerationInput input;
    input.provider = inferProviderFromPath(req);
    if (req) {
        input.method = req->methodString();
        input.path = req->path();
    }
    input.headers = requestHeaders(req);
    if (req && !req->getBody().empty()) {
        input.jsonBody = req->getBody();
    } else {
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        input.jsonBody = Json::writeString(writer, body);
    }
    return input;
}

HttpResponsePtr makeUseCaseError(const aiapi::Error& error)
{
    auto response = ctl::makeError(
        static_cast<HttpStatusCode>(error.httpStatus),
        error.type.empty() ? "internal_error" : error.type,
        error.message.empty() ? "Internal server error" : error.message,
        error.code);
    if (error.retryAfterSeconds > 0) {
        response->addHeader("Retry-After", std::to_string(error.retryAfterSeconds));
    }
    return response;
}

HttpResponsePtr unavailableResponse()
{
    aiapi::Error error;
    error.httpStatus = 503;
    error.type = "service_unavailable";
    error.message = "Server is shutting down";
    error.code = "shutting_down";
    return makeUseCaseError(error);
}

platform::ErrorCode toGenerationErrorCode(const aiapi::Error& error)
{
    if (error.httpStatus == 400) return platform::ErrorCode::BadRequest;
    if (error.httpStatus == 401) return platform::ErrorCode::Unauthorized;
    if (error.httpStatus == 403) return platform::ErrorCode::Forbidden;
    if (error.httpStatus == 404) return platform::ErrorCode::NotFound;
    if (error.httpStatus == 409) return platform::ErrorCode::Conflict;
    if (error.httpStatus == 429) return platform::ErrorCode::RateLimited;
    if (error.httpStatus == 504 || error.httpStatus == 408) return platform::ErrorCode::Timeout;
    if (error.httpStatus == 502) return platform::ErrorCode::ProviderError;
    if (error.httpStatus == 499) return platform::ErrorCode::Cancelled;
    return platform::ErrorCode::Internal;
}

void emitUseCaseErrorToSink(const aiapi::Error& error, IResponseSink& sink)
{
    generation::Error event;
    event.code = toGenerationErrorCode(error);
    event.message = error.message.empty() ? "Internal server error" : error.message;
    event.detail = error.detail;
    event.providerCode = error.providerCode;
    sink.onEvent(event);
}

void finishJsonGeneration(
    const std::shared_ptr<std::function<void(const HttpResponsePtr&)>>& callback,
    const std::shared_ptr<JsonResponseState>& state,
    const aiapi::GenerationResult& result)
{
    if (!result.succeeded()) {
        ctl::respondInLoop(callback, makeUseCaseError(*result.error));
        return;
    }
    if (state && state->response) {
        state->response->setStatusCode(static_cast<HttpStatusCode>(state->status));
        state->response->setContentTypeString("application/json; charset=utf-8");
        ctl::respondInLoop(callback, state->response);
        return;
    }
    ctl::respondInLoop(callback, ctl::makeError(
        k500InternalServerError, "internal_error", "Failed to generate response"));
}

bool parseStoredResponse(const std::string& serialized, Json::Value& out)
{
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(serialized);
    return Json::parseFromStream(builder, stream, &out, &errors);
}

}  // namespace

aiapi::IAiApiUseCase* AiApiController::useCase_ = nullptr;

void AiApiController::setUseCase(aiapi::IAiApiUseCase* useCase)
{
    useCase_ = useCase;
}

void AiApiController::chaynsapichat(
    const HttpRequestPtr &req,
    std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[AI] ";
    logSafeRequestMetadata(req, "chat.completions");

    std::shared_ptr<Json::Value> json;
    if (!ctl::parseJsonOrError(req, callback, json)) return;
    if ((*json)["messages"].empty()) {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error",
                       "Messages array cannot be empty");
        return;
    }

    const bool stream = (*json).get("stream", false).asBool();
    const auto input = generationInput(req, *json);
    auto* const useCase = useCase_;

    if (!stream) {
        auto cb = std::make_shared<std::function<void(const HttpResponsePtr&)>>(
            std::move(callback));
        if (!useCase) {
            ctl::respondInLoop(cb, unavailableResponse());
            return;
        }

        auto state = std::make_shared<JsonResponseState>();
        aiapi::IAiApiUseCase::ResponseBinding binding;
        binding.jsonResponse = [state](const Json::Value& response, int status) {
            state->response = HttpResponse::newHttpJsonResponse(response);
            state->status = status;
        };
        const auto submission = useCase->submitGeneration(
            input,
            std::move(binding),
            [cb, state](const aiapi::GenerationResult& result,
                        const std::shared_ptr<IResponseSink>&) {
                finishJsonGeneration(cb, state, result);
            });
        if (!submission.accepted()) {
            ctl::respondInLoop(cb, makeUseCaseError(*submission.error));
        }
        return;
    }

    auto response = HttpResponse::newAsyncStreamResponse(
        [input, useCase](ResponseStreamPtr streamResponse) mutable {
            if (!streamResponse) {
                LOG_WARN << "[AI] ";
                return;
            }
            const auto streamBridge = IoLoopResponseStream::create(std::move(streamResponse));
            if (!streamBridge) {
                LOG_WARN << "[AI] IO ";
                return;
            }
            if (!useCase) {
                streamBridge->close();
                return;
            }

            aiapi::IAiApiUseCase::ResponseBinding binding;
            binding.stream = true;
            binding.streamWriter = [streamBridge](const std::string& chunk) {
                return streamBridge->send(chunk);
            };
            binding.close = [streamBridge] { streamBridge->close(); };
            const auto submission = useCase->submitGeneration(
                std::move(input),
                std::move(binding),
                [](const aiapi::GenerationResult& result,
                   const std::shared_ptr<IResponseSink>& sink) {
                    if (!result.succeeded() && sink && sink->isValid()) {
                        emitUseCaseErrorToSink(*result.error, *sink);
                        sink->onClose();
                    }
                });
            if (!submission.accepted()) {
                LOG_WARN << "[AI] ("
                         << static_cast<int>(submission.outcome) << ")，";
                streamBridge->close();
            }
        },
        true);

    response->setContentTypeString("text/event-stream; charset=utf-8");
    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("Connection", "keep-alive");
    response->addHeader("X-Accel-Buffering", "no");
    response->addHeader("Keep-Alive", "timeout=60");
    callback(response);
}

void AiApiController::chaynsapimodels(
    const HttpRequestPtr &req,
    std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[AI] ";
    const auto provider = inferProviderFromPath(req);
    auto* const useCase = useCase_;
    const auto result = useCase ? useCase->modelCatalog(provider)
                                : aiapi::ModelCatalogResult{};
    if (!result.found()) {
        ctl::sendError(callback, k500InternalServerError, "provider_not_found",
                       "Provider not found: " + provider);
        return;
    }
    ctl::sendJson(callback, providermodelcodec::toJson(result.catalog));
}

void AiApiController::retiredNexos(
    const HttpRequestPtr &req,
    std::function<void(const HttpResponsePtr &)> &&callback)
{
    retired_provider::respondNexosTombstone(req, std::move(callback));
}

void AiApiController::retiredNexosWithId(
    const HttpRequestPtr &req,
    std::function<void(const HttpResponsePtr &)> &&callback,
    std::string)
{
    retired_provider::respondNexosTombstone(req, std::move(callback));
}

void AiApiController::responsesCreate(
    const HttpRequestPtr &req,
    std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[AI] Responses ";
    logSafeRequestMetadata(req, "responses.create");

    std::shared_ptr<Json::Value> json;
    if (!ctl::parseJsonOrError(req, callback, json, "invalid_json",
                               "Invalid JSON in request body")) return;
    if ((*json).get("input", Json::Value()).empty() &&
        (*json).get("messages", Json::Value()).empty()) {
        ctl::sendError(callback, k400BadRequest, "missing_input", "Input cannot be empty");
        return;
    }

    const bool stream = (*json).get("stream", false).asBool();
    const auto input = generationInput(req, *json);
    auto* const useCase = useCase_;

    if (!stream) {
        auto cb = std::make_shared<std::function<void(const HttpResponsePtr&)>>(
            std::move(callback));
        if (!useCase) {
            ctl::respondInLoop(cb, unavailableResponse());
            return;
        }

        auto state = std::make_shared<JsonResponseState>();
        aiapi::IAiApiUseCase::ResponseBinding binding;
        binding.jsonResponse = [state](const Json::Value& response, int status) {
            state->response = HttpResponse::newHttpJsonResponse(response);
            state->status = status;
        };
        const auto submission = useCase->submitGeneration(
            input,
            std::move(binding),
            [cb, state](const aiapi::GenerationResult& result,
                        const std::shared_ptr<IResponseSink>&) {
                finishJsonGeneration(cb, state, result);
            });
        if (!submission.accepted()) {
            ctl::respondInLoop(cb, makeUseCaseError(*submission.error));
        }
        return;
    }

    auto response = HttpResponse::newAsyncStreamResponse(
        [input, useCase](ResponseStreamPtr streamResponse) mutable {
            if (!streamResponse) {
                LOG_WARN << "[AI] Responses ";
                return;
            }
            const auto streamBridge = IoLoopResponseStream::create(std::move(streamResponse));
            if (!streamBridge) {
                LOG_WARN << "[AI] Responses IO ";
                return;
            }
            if (!useCase) {
                streamBridge->close();
                return;
            }

            aiapi::IAiApiUseCase::ResponseBinding binding;
            binding.stream = true;
            binding.streamWriter = [streamBridge](const std::string& chunk) {
                return streamBridge->send(chunk);
            };
            binding.close = [streamBridge] { streamBridge->close(); };
            const auto submission = useCase->submitGeneration(
                std::move(input),
                std::move(binding),
                [](const aiapi::GenerationResult& result,
                   const std::shared_ptr<IResponseSink>& sink) {
                    if (!result.succeeded() && sink && sink->isValid()) {
                        emitUseCaseErrorToSink(*result.error, *sink);
                        sink->onClose();
                    }
                });
            if (!submission.accepted()) {
                LOG_WARN << "[AI] Responses ("
                         << static_cast<int>(submission.outcome) << ")，";
                streamBridge->close();
            }
        },
        true);

    response->setContentTypeString("text/event-stream; charset=utf-8");
    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("Connection", "keep-alive");
    response->addHeader("X-Accel-Buffering", "no");
    response->addHeader("Keep-Alive", "timeout=60");
    callback(response);
}

void AiApiController::responsesGet(
    const HttpRequestPtr &,
    std::function<void(const HttpResponsePtr &)> &&callback,
    std::string responseId)
{
    LOG_INFO << "[AI] ResponsesGet - ID：" << responseId;
    auto* const useCase = useCase_;
    const auto result = useCase ? useCase->getResponse(responseId)
                                : aiapi::StoredResponseResult{};
    if (result.outcome == aiapi::StoredResponseOutcome::NotFound) {
        ctl::sendError(callback, k404NotFound, "invalid_request_error", "Response not found",
                       "response_not_found");
        return;
    }
    if (result.outcome != aiapi::StoredResponseOutcome::Found) {
        ctl::sendError(callback, k500InternalServerError, "internal_error",
                       "Stored response is invalid");
        return;
    }

    Json::Value stored;
    if (!parseStoredResponse(result.jsonBody, stored)) {
        ctl::sendError(callback, k500InternalServerError, "internal_error",
                       "Stored response is invalid");
        return;
    }
    ctl::sendJson(callback, stored);
}

void AiApiController::responsesDelete(
    const HttpRequestPtr &,
    std::function<void(const HttpResponsePtr &)> &&callback,
    std::string responseId)
{
    LOG_INFO << "[AI] Responses - ID：" << responseId;
    auto* const useCase = useCase_;
    const auto result = useCase ? useCase->deleteResponse(responseId)
                                : aiapi::DeleteResponseResult{};
    if (!result.deleted()) {
        ctl::sendError(callback, k404NotFound, "invalid_request_error", "Response not found",
                       "response_not_found");
        return;
    }

    Json::Value response;
    response["id"] = responseId;
    response["object"] = "response";
    response["deleted"] = true;
    ctl::sendJson(callback, response);
}

std::string AiApiController::generateClientId(const HttpRequestPtr &req)
{
    std::string clientIp = req->getPeerAddr().toIp();
    std::string userAgent = req->getHeader("User-Agent");
    return clientIp + "_" + std::to_string(std::hash<std::string>{}(userAgent));
}

bool AiApiController::isCreateNewSession(const HttpRequestPtr &req)
{
    auto json = req->getJsonObject();
    return json && json->isMember("new_session") && (*json)["new_session"].asBool();
}
