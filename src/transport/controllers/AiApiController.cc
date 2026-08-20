#include <transport/controllers/AiApiController.h>

#include <transport/controllers/ControllerUtils.h>
#include <transport/controllers/RetiredProviderTombstone.h>
#include <transport/controllers/codecs/ProviderModelCatalogJsonCodec.h>
#include <application/generation/contracts/IResponseSink.h>
#include <application/generation/protocol/claude/ClaudeErrorFormatter.h>
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

HttpResponsePtr makeClaudeError(const aiapi::Error& error)
{
    const auto body = generation::protocol::claude::formatApiError(error);
    auto response = HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(static_cast<HttpStatusCode>(error.httpStatus));
    if (error.retryAfterSeconds > 0) {
        response->addHeader("Retry-After", std::to_string(error.retryAfterSeconds));
    }
    return response;
}

std::string makeClaudeSseError(const aiapi::Error& error)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return "event: error\ndata: " +
        Json::writeString(writer, generation::protocol::claude::formatApiError(error)) +
        "\n\n";
}

HttpResponsePtr unavailableClaudeResponse()
{
    aiapi::Error error;
    error.httpStatus = 503;
    error.type = "service_unavailable";
    error.message = "Server is shutting down";
    error.code = "shutting_down";
    return makeClaudeError(error);
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

void finishClaudeJsonGeneration(
    const std::shared_ptr<std::function<void(const HttpResponsePtr&)>>& callback,
    const std::shared_ptr<JsonResponseState>& state,
    const aiapi::GenerationResult& result)
{
    if (state && state->response) {
        state->response->setStatusCode(static_cast<HttpStatusCode>(state->status));
        state->response->setContentTypeString("application/json; charset=utf-8");
        ctl::respondInLoop(callback, state->response);
        return;
    }
    if (!result.succeeded()) {
        ctl::respondInLoop(callback, makeClaudeError(*result.error));
        return;
    }
    aiapi::Error error;
    error.message = "Failed to generate response";
    ctl::respondInLoop(callback, makeClaudeError(error));
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

/**
 * @brief 处理 OpenAI Chat Completions 兼容请求。
 *
 * 该处理器同时绑定 `/chaynsapi/v1/chat/completions` 和
 * `/retoolapi/v1/chat/completions`。控制器只负责 HTTP 输入校验、建立 JSON/SSE
 * 输出通道以及把任务提交给 IAiApiUseCase；协议适配、能力检查、后台排队、
 * Provider 调用和生成事件编码均由 UseCase 及其下游流水线完成。
 *
 * 主流程：记录安全元数据 -> 解析并校验 JSON -> 根据 `stream` 分流：
 * - 非流式：后台生成完整 JSON，完成后切回 Drogon IO 线程发送一次响应；
 * - 流式：先返回 SSE 响应，再通过线程安全的流桥接器逐块写入并最终关闭连接。
 *
 * C++ 语法：`AiApiController::` 表示该函数属于 AiApiController；`const T&`
 * 表示只读引用、避免复制；`T&&` 是右值引用，使一次性的 Drogon 回调可以被
 * `std::move` 转移到异步任务中。函数返回 `void`，HTTP 结果通过 callback 交回。
 */
void AiApiController::chaynsapichat(
    const HttpRequestPtr &req,
    std::function<void(const HttpResponsePtr &)> &&callback)
{
    // LOG_INFO 使用重载后的 `<<` 流插入运算符写入 INFO 级别日志。
    LOG_INFO << "[AI] ";

    // 仅记录方法、路径、正文大小以及敏感请求头是否存在，不输出正文、令牌或 Cookie 的实际内容。
    logSafeRequestMetadata(req, "chat.completions");

    // shared_ptr 是共享所有权智能指针；当前为空，parseJsonOrError 成功后令其指向 Json::Value。
    std::shared_ptr<Json::Value> json;

    // `!` 是逻辑非；解析失败时辅助函数已发送 400 响应，所以用提前 return 终止后续处理。
    if (!ctl::parseJsonOrError(req, callback, json)) return;

    // `*json` 解引用智能指针，`["messages"]` 访问 JSON 字段；字段缺失或空数组时 empty() 为 true。
    if ((*json)["messages"].empty()) {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error",
                       "Messages array cannot be empty");
        return;
    }

    // get("stream", false) 表示字段缺失时默认非流式，asBool() 将 Json::Value 转为 bool。
    const bool stream = (*json).get("stream", false).asBool();

    // auto 由编译器推导为 aiapi::GenerationInput；辅助函数保存 Provider、路径、方法、请求头和原始 JSON。
    const auto input = generationInput(req, *json);

    // 实际类型为 IAiApiUseCase* const：指针本身不能改指向，但仍可调用目标对象的非 const 方法。
    // 复制到局部变量后，可安全地按值捕获进下面的异步 Lambda；该裸指针不拥有 UseCase 生命周期。
    auto* const useCase = useCase_;

    // -------------------- 非流式 JSON 分支 --------------------
    if (!stream) {
        // callback 是有名字的右值引用，在表达式中仍是左值；std::move 才允许转移其内部资源。
        // shared_ptr 延长回调生命周期，并使后台完成 Lambda 与 IO 线程调度代码可以共同持有它。
        auto cb = std::make_shared<std::function<void(const HttpResponsePtr&)>>(
            std::move(callback));

        // UseCase 未注入或已在停机阶段撤销时，构造 503，并通过 respondInLoop 切回 Drogon IO 线程发送。
        if (!useCase) {
            ctl::respondInLoop(cb, unavailableResponse());
            return;
        }

        // JsonResponseState 在“协议 Sink 生成响应”和“完成回调发送响应”之间共享 HTTP 响应及状态码。
        auto state = std::make_shared<JsonResponseState>();

        // `aiapi::IAiApiUseCase::ResponseBinding` 使用作用域解析符访问命名空间、类及其嵌套类型。
        // 非流式绑定只需提供 jsonResponse；stream 保持默认 false。
        aiapi::IAiApiUseCase::ResponseBinding binding;

        // `[state]` 表示 Lambda 按值捕获 shared_ptr，引用计数增加，保证异步执行时状态仍然有效。
        // 协议 JSON Sink 完成编码后调用这里；这里只暂存响应，尚未调用最外层 HTTP callback。
        binding.jsonResponse = [state](const Json::Value& response, int status) {
            state->response = HttpResponse::newHttpJsonResponse(response);
            state->status = status;
        };

        // submitGeneration 先同步完成协议适配、能力校验和队列准入，再由后台任务执行实际生成。
        // 因此 submission 表示“是否成功提交”，而下面的 GenerationResult 表示“后台执行是否成功”。
        const auto submission = useCase->submitGeneration(
            input,
            // binding 后续不再使用，移动可避免复制其中保存的 std::function。
            std::move(binding),
            // `[cb, state]` 按值捕获两个 shared_ptr，使它们一直存活到后台完成回调结束。
            [cb, state](const aiapi::GenerationResult& result,
                        // 第二个形参故意不命名：接口会传入 Sink，但非流式收尾逻辑不需要直接使用它。
                        const std::shared_ptr<IResponseSink>&) {
                // 失败时发送标准错误；成功时发送 state 中的完整 JSON；响应操作会被排回 IO 线程。
                finishJsonGeneration(cb, state, result);
            });

        // accepted() 为 false 表示任务没有进入后台队列，例如请求无效、队列满或正在停机。
        if (!submission.accepted()) {
            // error 是 optional；`*submission.error` 取出错误值。此处依赖“拒绝必带错误”的接口约定。
            ctl::respondInLoop(cb, makeUseCaseError(*submission.error));
        }
        return;
    }

    // -------------------- 流式 SSE 分支 --------------------
    // newAsyncStreamResponse 创建基于 HTTP chunked transfer 的异步响应；回调会在流可写时收到流对象。
    auto response = HttpResponse::newAsyncStreamResponse(
        // input 和 useCase 按值捕获。Lambda 默认的 operator() 是 const；mutable 允许随后移动 input。
        [input, useCase](ResponseStreamPtr streamResponse) mutable {
            // 智能指针支持布尔判断；空流无法发送任何 SSE 数据。
            if (!streamResponse) {
                LOG_WARN << "[AI] ";
                return;
            }

            // 把 Drogon 流移动给桥接器。桥接器确保 send、close 和销毁都发生在 TCP 所属事件循环线程。
            const auto streamBridge = IoLoopResponseStream::create(std::move(streamResponse));
            if (!streamBridge) {
                LOG_WARN << "[AI] IO ";
                return;
            }

            // 流已开始建立后不再改发普通 JSON 503；UseCase 不可用时只能安全关闭流。
            if (!useCase) {
                streamBridge->close();
                return;
            }

            aiapi::IAiApiUseCase::ResponseBinding binding;

            // 注意：ResponseBinding::stream 默认是 false，本分支当前没有显式把它改为 true。
            // 下游响应工厂会依据该字段选择 JSON Sink 或 SSE Sink，这是理解当前实际行为的关键。

            // 协议 Sink 每产生一个 SSE 数据块就调用该 Lambda；返回 false 表示流已关闭或发送失败。
            binding.streamWriter = [streamBridge](const std::string& chunk) {
                return streamBridge->send(chunk);
            };

            // 省略空圆括号的无参 Lambda；正常结束或失败时由下游调用以关闭 chunked 响应。
            binding.close = [streamBridge] { streamBridge->close(); };

            const auto submission = useCase->submitGeneration(
                // Lambda 带 mutable，因此捕获的 input 可以被移动；提交后本分支不再访问它。
                std::move(input),
                std::move(binding),
                // 空捕获列表 `[]` 表示完成回调不依赖外部局部变量。
                [](const aiapi::GenerationResult& result,
                   const std::shared_ptr<IResponseSink>& sink) {
                    // `&&` 短路求值：只有 sink 非空时才调用 isValid()，避免空指针解引用。
                    if (!result.succeeded() && sink && sink->isValid()) {
                        // 将 UseCase 错误转成统一生成事件，由协议 Sink 编码为对应的流式错误格式。
                        emitUseCaseErrorToSink(*result.error, *sink);
                        sink->onClose();
                    }
                    // 成功路径不在此重复关闭；GenerationPipeline 的统一出口会发送完成事件并关闭 Sink。
                });

            // 这是同步的队列准入失败，此时可能尚未创建协议 Sink，只记录结果并关闭底层流。
            if (!submission.accepted()) {
                LOG_WARN << "[AI] ("
                         // enum class 不会隐式转成整数，必须使用 static_cast 显式转换后才能写入日志。
                         << static_cast<int>(submission.outcome) << ")，";
                streamBridge->close();
            }
        },
        // 第二个参数为 disableKickoffTimeout；true 关闭默认启动超时，适合耗时较长的 AI SSE 请求。
        true);

    // SSE 必需及常用响应头：声明事件流、禁用缓存和代理缓冲，并尽量维持长连接。
    response->setContentTypeString("text/event-stream; charset=utf-8");
    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("Connection", "keep-alive");
    response->addHeader("X-Accel-Buffering", "no");
    response->addHeader("Keep-Alive", "timeout=60");

    // 流式模式先把响应头和异步响应对象交给 Drogon，正文随后由 streamWriter 分块发送。
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

void AiApiController::messagesCreate(
    const HttpRequestPtr &req,
    std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[AI] Claude Messages";
    logSafeRequestMetadata(req, "messages.create");

    auto json = req ? req->getJsonObject() : nullptr;
    if (!json) {
        aiapi::Error error;
        error.httpStatus = 400;
        error.type = "invalid_request_error";
        error.message = "Invalid JSON in request body";
        callback(makeClaudeError(error));
        return;
    }

    const bool stream = (*json).get("stream", false).asBool();
    const auto input = generationInput(req, *json);
    auto* const useCase = useCase_;

    if (!stream) {
        auto cb = std::make_shared<std::function<void(const HttpResponsePtr&)>>(
            std::move(callback));
        if (!useCase) {
            ctl::respondInLoop(cb, unavailableClaudeResponse());
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
                finishClaudeJsonGeneration(cb, state, result);
            });
        if (!submission.accepted()) {
            ctl::respondInLoop(cb, makeClaudeError(*submission.error));
        }
        return;
    }

    auto response = HttpResponse::newAsyncStreamResponse(
        [input, useCase](ResponseStreamPtr streamResponse) mutable {
            if (!streamResponse) {
                LOG_WARN << "[AI] Claude Messages stream response unavailable";
                return;
            }
            const auto streamBridge = IoLoopResponseStream::create(std::move(streamResponse));
            if (!streamBridge) {
                LOG_WARN << "[AI] Claude Messages IO bridge unavailable";
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
                LOG_WARN << "[AI] Claude Messages submission rejected ("
                         << static_cast<int>(submission.outcome) << ")"
                         << (submission.error.has_value()
                                 ? ": " + submission.error->message
                                 : std::string());
                if (submission.error.has_value()) {
                    streamBridge->send(makeClaudeSseError(*submission.error));
                }
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
