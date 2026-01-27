#include "ResponsesJsonSink.h"
#include <chrono>

ResponsesJsonSink::ResponsesJsonSink(
    ResponseCallback responseCallback,
    const std::string& responseId,
    const std::string& model,
    const std::string& internalSessionId,
    int inputTokensEstimated
) : responseCallback_(std::move(responseCallback)),
    responseId_(responseId),
    model_(model),
    internalSessionId_(internalSessionId),
    inputTokensEstimated_(inputTokensEstimated)
{
    createdAt_ = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

void ResponsesJsonSink::onEvent(const generation::GenerationEvent& event) {
    if (closed_) return;

    std::visit([this](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;

        if constexpr (std::is_same_v<T, generation::Started>) {
            // JSON sink 不需要在 Started 时做任何事
        }
        else if constexpr (std::is_same_v<T, generation::OutputTextDelta>) {
            collectedText_ += arg.delta;
        }
        else if constexpr (std::is_same_v<T, generation::OutputTextDone>) {
            if (collectedText_.empty()) {
                collectedText_ = arg.text;
            }
        }
        else if constexpr (std::is_same_v<T, generation::ToolCallDone>) {
            toolCalls_.push_back(arg);
        }
        else if constexpr (std::is_same_v<T, generation::Usage>) {
            // usage 会在 Completed 里出现（GenerationService 会把 usage 放进 Completed）
        }
        else if constexpr (std::is_same_v<T, generation::Completed>) {
            if (arg.usage.has_value()) {
                usage_ = *arg.usage;
            }
        }
        else if constexpr (std::is_same_v<T, generation::Error>) {
            hasError_ = true;
            errorMessage_ = arg.message;
            errorType_ = generation::errorCodeToString(arg.code);
            statusCode_ = generation::errorCodeToHttpStatus(arg.code);
        }
    }, event);
}

void ResponsesJsonSink::onClose() {
    if (closed_) return;
    closed_ = true;

    if (responseCallback_) {
        Json::Value response = buildResponse();
        responseCallback_(response, statusCode_);
    }
}

bool ResponsesJsonSink::isValid() const {
    return !closed_;
}

Json::Value ResponsesJsonSink::buildResponse() {
    if (hasError_) {
        Json::Value error;
        error["error"]["message"] = errorMessage_;
        error["error"]["type"] = errorType_;
        error["error"]["code"] = errorType_;
        return error;
    }

    Json::Value response;
    response["id"] = responseId_;
    response["object"] = "response";
    response["created_at"] = static_cast<Json::Int64>(createdAt_);
    response["model"] = model_;
    response["status"] = "completed";

    // 内部字段：供 Session 层存储/续聊映射
    response["_internal_session_id"] = internalSessionId_;

    // output
    Json::Value outputArray(Json::arrayValue);

    Json::Value messageOutput;
    messageOutput["type"] = "message";
    messageOutput["id"] = "msg_" + responseId_;
    messageOutput["status"] = "completed";
    messageOutput["role"] = "assistant";

    // content
    Json::Value contentArray(Json::arrayValue);
    if (!collectedText_.empty()) {
        Json::Value textContent;
        textContent["type"] = "output_text";
        textContent["text"] = collectedText_;
        contentArray.append(textContent);
    }
    messageOutput["content"] = contentArray;

    // tool_calls（如果有）
    if (!toolCalls_.empty()) {
        Json::Value toolCallsJson(Json::arrayValue);
        for (const auto& tc : toolCalls_) {
            Json::Value call;
            call["id"] = tc.id;
            call["type"] = "function";

            Json::Value func;
            func["name"] = tc.name;
            func["arguments"] = tc.arguments;
            call["function"] = func;

            toolCallsJson.append(call);
        }
        messageOutput["tool_calls"] = toolCallsJson;
    }

    outputArray.append(messageOutput);
    response["output"] = outputArray;

    // usage
    Json::Value usage;
    if (usage_.has_value()) {
        usage["input_tokens"] = usage_->inputTokens;
        usage["output_tokens"] = usage_->outputTokens;
        usage["total_tokens"] = usage_->totalTokens;
    } else {
        int inputTokens = inputTokensEstimated_;
        int outputTokens = static_cast<int>(collectedText_.length() / 4);
        usage["input_tokens"] = inputTokens;
        usage["output_tokens"] = outputTokens;
        usage["total_tokens"] = inputTokens + outputTokens;
    }
    response["usage"] = usage;

    return response;
}
