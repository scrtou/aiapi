#include "ChatJsonSink.h"
#include <chrono>
#include <random>

ChatJsonSink::ChatJsonSink(
    ResponseCallback responseCallback,
    const std::string& model
) : responseCallback_(std::move(responseCallback)),
    model_(model),
    completionId_(generateCompletionId())
{
}

void ChatJsonSink::onEvent(const generation::GenerationEvent& event) {
    if (closed_) {
        return;
    }
    
    std::visit([this](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        
        if constexpr (std::is_same_v<T, generation::Started>) {
            // JSON sink 不需要在 Started 时做任何事
        }
        else if constexpr (std::is_same_v<T, generation::OutputTextDelta>) {
            // 收集增量文本
            collectedText_ += arg.delta;
        }
        else if constexpr (std::is_same_v<T, generation::OutputTextDone>) {
            // 使用完整文本（如果之前没有收集到增量）
            if (collectedText_.empty()) {
                collectedText_ = arg.text;
            }
        }
        else if constexpr (std::is_same_v<T, generation::ToolCallDone>) {
            toolCalls_.push_back(arg);
        }
        else if constexpr (std::is_same_v<T, generation::Usage>) {
            // 存储 usage 信息（供 Chat Completions 的 usage 字段使用）
            usage_ = arg;
            if (usage_->totalTokens == 0) {
                usage_->totalTokens = usage_->inputTokens + usage_->outputTokens;
            }
        }
        else if constexpr (std::is_same_v<T, generation::Completed>) {
            finishReason_ = arg.finishReason.empty() ? "stop" : arg.finishReason;
            if (arg.usage.has_value()) {
                usage_ = arg.usage;
                if (usage_->totalTokens == 0) {
                    usage_->totalTokens = usage_->inputTokens + usage_->outputTokens;
                }
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

void ChatJsonSink::onClose() {
    if (!closed_) {
        closed_ = true;
        
        if (responseCallback_) {
            Json::Value response = buildResponse();
            responseCallback_(response, statusCode_);
        }
    }
}

bool ChatJsonSink::isValid() const {
    return !closed_;
}

Json::Value ChatJsonSink::buildResponse() {
    if (hasError_) {
        Json::Value error;
        error["error"]["message"] = errorMessage_;
        error["error"]["type"] = errorType_;
        error["error"]["code"] = errorType_;
        return error;
    }
    
    Json::Value response;
    response["id"] = completionId_;
    response["object"] = "chat.completion";
    response["created"] = static_cast<Json::Int64>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    response["model"] = model_;

    // Chat Completions usage（简单实现：优先使用 Session/Provider 上报的 usage；没有则返回 0）
    {
        int promptTokens = 0;
        int completionTokens = 0;
        int totalTokens = 0;
        if (usage_.has_value()) {
            promptTokens = usage_->inputTokens;
            completionTokens = usage_->outputTokens;
            totalTokens = usage_->totalTokens;
            if (totalTokens == 0) {
                totalTokens = promptTokens + completionTokens;
            }
        }
        Json::Value usageJson;
        usageJson["prompt_tokens"] = promptTokens;
        usageJson["completion_tokens"] = completionTokens;
        usageJson["total_tokens"] = totalTokens;
        response["usage"] = usageJson;
    }
    
    Json::Value choice;
    choice["index"] = 0;
    
    Json::Value message;
    message["role"] = "assistant";
    if (!toolCalls_.empty()) {
        // OpenAI ChatCompletions: when tool_calls are present, content is usually null.
        // Keep text only if we actually have remaining non-tool text.
        if (collectedText_.empty()) {
            message["content"] = Json::nullValue;
        } else {
            message["content"] = collectedText_;
        }

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
        message["tool_calls"] = toolCallsJson;
    } else {
        message["content"] = collectedText_;
    }
    
    choice["message"] = message;
    choice["finish_reason"] = finishReason_;
    
    response["choices"] = Json::Value(Json::arrayValue);
    response["choices"].append(choice);
    
    return response;
}

std::string ChatJsonSink::generateCompletionId() {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    
    return "chatcmpl-" + std::to_string(timestamp) + std::to_string(dis(gen));
}
