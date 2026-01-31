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
            sentText_ = true;
        }
        else if constexpr (std::is_same_v<T, generation::OutputTextDone>) {
            // 如果之前没有发送过文本增量，发送完整文本
            if (!sentText_ && !arg.text.empty()) {
                std::string json = buildChunkJson(arg.text, "", firstChunk_);
                sendSseEvent(json);
                firstChunk_ = false;
                sentText_ = true;
            }
        }
        else if constexpr (std::is_same_v<T, generation::ToolCallDone>) {
            // 发送 tool_calls delta
            // IMPORTANT: For ChatCompletions streaming, some clients only execute tools when
            // tool_calls and finish_reason="tool_calls" appear together in the same chunk.
            // Emit finish_reason here (and avoid emitting a separate trailing finish chunk).
            std::string json = buildToolCallChunkJson(arg, "tool_calls", firstChunk_);
            sendSseEvent(json);
            firstChunk_ = false;
        }
        else if constexpr (std::is_same_v<T, generation::Usage>) {
            LOG_DEBUG << "[聊天SSE] 令牌用量: 输入=" << arg.inputTokens
                      << ", 输出=" << arg.outputTokens;
            usage_ = arg;
            if (usage_->totalTokens == 0) {
                usage_->totalTokens = usage_->inputTokens + usage_->outputTokens;
            }
        }
        else if constexpr (std::is_same_v<T, generation::Completed>) {
            // Completed 可能携带 usage（优先使用 Completed 的 usage）
            if (arg.usage.has_value()) {
                usage_ = arg.usage;
                if (usage_->totalTokens == 0) {
                    usage_->totalTokens = usage_->inputTokens + usage_->outputTokens;
                }
            }

            // When tool calls are present, we already emitted a chunk that contains BOTH
            // tool_calls and finish_reason="tool_calls". Do not emit an extra trailing
            // empty finish chunk that would separate them again.
            const std::string finish = arg.finishReason.empty() ? "stop" : arg.finishReason;
            if (finish != "tool_calls") {
                std::string json = buildChunkJson("", finish, false);
                sendSseEvent(json);
            }

            // 发送 usage chunk（非标准 OpenAI chunk，但满足“流式返回 usage”的需求）
            if (usage_.has_value()) {
                sendSseEvent(buildUsageChunkJson(*usage_));
            }

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

std::string ChatSseSink::buildToolCallChunkJson(
    const generation::ToolCallDone& toolCall,
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

    Json::Value toolCallsJson(Json::arrayValue);
    Json::Value call;
    call["index"] = toolCall.index;
    call["id"] = toolCall.id;
    call["type"] = "function";

    Json::Value func;
    func["name"] = toolCall.name;
    func["arguments"] = toolCall.arguments;
    call["function"] = func;

    toolCallsJson.append(call);
    deltaJson["tool_calls"] = toolCallsJson;

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

std::string ChatSseSink::buildUsageChunkJson(const generation::Usage& usage) {
    Json::Value chunk;
    chunk["id"] = completionId_;
    chunk["object"] = "chat.completion.chunk";
    chunk["created"] = static_cast<Json::Int64>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    chunk["model"] = model_;

    // 非标准：为了兼容“流式返回 usage”，这里 choices 发送空数组
    chunk["choices"] = Json::Value(Json::arrayValue);

    int total = usage.totalTokens;
    if (total == 0) {
        total = usage.inputTokens + usage.outputTokens;
    }

    Json::Value usageJson;
    usageJson["prompt_tokens"] = usage.inputTokens;
    usageJson["completion_tokens"] = usage.outputTokens;
    usageJson["total_tokens"] = total;
    chunk["usage"] = usageJson;

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
