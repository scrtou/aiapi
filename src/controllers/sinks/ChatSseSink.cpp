#include "ChatSseSink.h"
#include <json/json.h>
#include <chrono>
#include <random>

using namespace drogon;

ChatSseSink::ChatSseSink(
    StreamCallback streamCallback,
    CloseCallback closeCallback,
    const std::string& model
) : streamCallback_(std::move(streamCallback)),
    closeCallback_(std::move(closeCallback)),
    model_(model),
    completionId_(generateCompletionId())
{
    LOG_DEBUG << "[聊天SSE] 已创建, 模型: " << model_ << ", ID: " << completionId_;
}

void ChatSseSink::onEvent(const generation::GenerationEvent& event) {
    if (closed_) {
        LOG_WARN << "[聊天SSE] 关闭后收到事件";
        return;
    }
    
    std::visit([this](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        
        if constexpr (std::is_same_v<T, generation::Started>) {
            LOG_DEBUG << "[聊天SSE] 开始事件, 响应ID: " << arg.responseId;
            // Chat SSE 不需要在 Started 时发送任何内容
        }
        else if constexpr (std::is_same_v<T, generation::OutputTextDelta>) {
            // 发送增量文本
            std::string json = buildChunkJson(arg.delta, "", firstChunk_);
            sendSseEvent(json);
            firstChunk_ = false;
        }
        else if constexpr (std::is_same_v<T, generation::OutputTextDone>) {
            // 如果之前没有发送过增量，发送完整文本
            if (firstChunk_) {
                std::string json = buildChunkJson(arg.text, "", true);
                sendSseEvent(json);
                firstChunk_ = false;
            }
        }
        else if constexpr (std::is_same_v<T, generation::Usage>) {
            LOG_DEBUG << "[聊天SSE] 令牌用量: 输入=" << arg.inputTokens 
                     << ", 输出=" << arg.outputTokens;
            // 可以选择在这里发送 usage 信息
        }
        else if constexpr (std::is_same_v<T, generation::Completed>) {
            // 发送带有 finish_reason 的最后一个 chunk
            std::string json = buildChunkJson("", arg.finishReason.empty() ? "stop" : arg.finishReason, false);
            sendSseEvent(json);
            sendDone();
        }
        else if constexpr (std::is_same_v<T, generation::Error>) {
            LOG_ERROR << "[聊天SSE] 错误: " << arg.message;
            // 构建错误响应
            Json::Value errorJson;
            errorJson["error"]["message"] = arg.message;
            errorJson["error"]["type"] = generation::errorCodeToString(arg.code);
            errorJson["error"]["code"] = generation::errorCodeToString(arg.code);
            
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            std::string errorStr = Json::writeString(writer, errorJson);
            sendSseEvent(errorStr);
        }
    }, event);
}

void ChatSseSink::onClose() {
    if (!closed_) {
        closed_ = true;
        LOG_DEBUG << "[聊天SSE] 正在关闭";
        if (closeCallback_) {
            closeCallback_();
        }
    }
}

bool ChatSseSink::isValid() const {
    return !closed_;
}

void ChatSseSink::sendSseEvent(const std::string& data) {
    if (closed_) return;
    
    std::string sseData = "data: " + data + "\n\n";
    if (streamCallback_) {
        if (!streamCallback_(sseData)) {
            LOG_WARN << "[聊天SSE] 流回调返回false";
            closed_ = true;
        }
    }
}

void ChatSseSink::sendDone() {
    if (closed_) return;
    
    std::string doneData = "data: [DONE]\n\n";
    if (streamCallback_) {
        streamCallback_(doneData);
    }
}

std::string ChatSseSink::buildChunkJson(
    const std::string& delta,
    const std::string& finishReason,
    bool includeRole
) {
    Json::Value chunk;
    chunk["id"] = completionId_;
    chunk["object"] = "chat.completion.chunk";
    chunk["created"] = static_cast<Json::Int64>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    chunk["model"] = model_;
    
    Json::Value choice;
    choice["index"] = 0;
    
    Json::Value deltaJson;
    if (includeRole) {
        deltaJson["role"] = "assistant";
    }
    if (!delta.empty()) {
        deltaJson["content"] = delta;
    }
    
    choice["delta"] = deltaJson;
    
    if (!finishReason.empty()) {
        choice["finish_reason"] = finishReason;
    } else {
        choice["finish_reason"] = Json::nullValue;
    }
    
    chunk["choices"] = Json::Value(Json::arrayValue);
    chunk["choices"].append(choice);
    
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, chunk);
}

std::string ChatSseSink::generateCompletionId() {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    
    return "chatcmpl-" + std::to_string(timestamp) + std::to_string(dis(gen));
}
