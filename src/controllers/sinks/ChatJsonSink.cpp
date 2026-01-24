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
        else if constexpr (std::is_same_v<T, generation::Usage>) {
            // 可以存储 usage 信息供后续使用
        }
        else if constexpr (std::is_same_v<T, generation::Completed>) {
            finishReason_ = arg.finishReason.empty() ? "stop" : arg.finishReason;
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
    
    Json::Value choice;
    choice["index"] = 0;
    
    Json::Value message;
    message["role"] = "assistant";
    message["content"] = collectedText_;
    
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
