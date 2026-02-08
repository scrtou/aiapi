#include "ResponsesSseSink.h"
#include <json/json.h>
#include <chrono>
#include <algorithm>

using namespace drogon;

ResponsesSseSink::ResponsesSseSink(
    StreamCallback streamCallback,
    CloseCallback closeCallback,
    const std::string& model
) : streamCallback_(std::move(streamCallback)),
    closeCallback_(std::move(closeCallback)),
    model_(model)
{
    createdAt_ = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    LOG_DEBUG << "[响应SSE] 已创建，模型：" << model_;
}

void ResponsesSseSink::onEvent(const generation::GenerationEvent& event) {
    if (closed_) {
        LOG_WARN << "[响应SSE] 关闭后收到事件";
        return;
    }
    
    std::visit([this](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        
        if constexpr (std::is_same_v<T, generation::Started>) {
            handleStarted(arg);
        }
        else if constexpr (std::is_same_v<T, generation::OutputTextDelta>) {
            handleOutputTextDelta(arg);
        }
        else if constexpr (std::is_same_v<T, generation::OutputTextDone>) {
            handleOutputTextDone(arg);
        }
        else if constexpr (std::is_same_v<T, generation::ToolCallDone>) {
            handleToolCallDone(arg);
        }
        else if constexpr (std::is_same_v<T, generation::Usage>) {
            LOG_DEBUG << "[响应SSE] 令牌用量： 输入=" << arg.inputTokens
                     << ", 输出=" << arg.outputTokens;
            // 信息会在 已完成 事件中包含
        }
        else if constexpr (std::is_same_v<T, generation::Completed>) {
            handleCompleted(arg);
        }
        else if constexpr (std::is_same_v<T, generation::Error>) {
            handleError(arg);
        }
    }, event);
}

void ResponsesSseSink::onClose() {
    if (!closed_) {
        closed_ = true;
        LOG_DEBUG << "[响应SSE] 正在关闭";
        if (closeCallback_) {
            closeCallback_();
        }
    }
}

bool ResponsesSseSink::isValid() const {
    return !closed_;
}

void ResponsesSseSink::sendSseEvent(const std::string& eventType, const std::string& data) {
    if (closed_) return;
    
    std::string sseData = "event: " + eventType + "\ndata: " + data + "\n\n";
    if (streamCallback_) {
        if (!streamCallback_(sseData)) {
            LOG_WARN << "[响应SSE] 流回调返回false";
            closed_ = true;
            if (closeCallback_) {
                closeCallback_();
            }
        }
    }
}

void ResponsesSseSink::sendSseEvent(const std::string& eventType, const Json::Value& data) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    writer["emitUTF8"] = true;
    sendSseEvent(eventType, Json::writeString(writer, data));
}

void ResponsesSseSink::handleStarted(const generation::Started& event) {
    LOG_DEBUG << "[响应SSE] 开始事件，响应ID：" << event.responseId;

    if (responseId_.empty()) {
        responseId_ = event.responseId;
    }
    if (model_.empty() && !event.model.empty()) {
        model_ = event.model;
    }
    
    // 1) 响应.已创建
    Json::Value createdEvent(Json::objectValue);
    createdEvent["type"] = "response.created";
    createdEvent["sequence_number"] = sequenceNumber_++;
    createdEvent["response"] = buildResponseObject("in_progress");
    sendSseEvent("response.created", createdEvent);


    Json::Value outputItem;
    outputItem["type"] = "message";
    outputItem["id"] = "msg_" + responseId_;
    outputItem["status"] = "in_progress";
    outputItem["role"] = "assistant";
    outputItem["content"] = Json::Value(Json::arrayValue);
    
    Json::Value outputItemEvent(Json::objectValue);
    outputItemEvent["type"] = "response.output_item.added";
    outputItemEvent["sequence_number"] = sequenceNumber_++;
    outputItemEvent["output_index"] = outputItemIndex_;
    outputItemEvent["item"] = outputItem;
    sendSseEvent("response.output_item.added", outputItemEvent);
}

void ResponsesSseSink::handleOutputTextDelta(const generation::OutputTextDelta& event) {
    sawDelta_ = true;
    // 累积文本
    outputText_ += event.delta;
    

    Json::Value deltaEvent(Json::objectValue);
    deltaEvent["type"] = "response.output_text.delta";
    deltaEvent["sequence_number"] = sequenceNumber_++;
    deltaEvent["item_id"] = "msg_" + responseId_;
    deltaEvent["output_index"] = outputItemIndex_;
    deltaEvent["content_index"] = 0;
    deltaEvent["delta"] = event.delta;
    
    sendSseEvent("response.output_text.delta", deltaEvent);
}

void ResponsesSseSink::handleOutputTextDone(const generation::OutputTextDone& event) {
    // 如果之前没有通过 发送，使用完整文本
    if (outputText_.empty()) {
        outputText_ = event.text;
    }

    // 当前项目没有上游真实 ；为了兼容 SSE 客户端，若未见 ，则把 拆分为多个 发送。
    if (!sawDelta_ && !outputText_.empty()) {
        auto utf8ChunkSize = [](const std::string& s, size_t pos, size_t maxBytes) -> size_t {
            if (pos >= s.size()) return 0;
            size_t remaining = s.size() - pos;
            size_t target = std::min(remaining, maxBytes);
            size_t end = pos + target;

            while (end < s.size() && end > pos &&
                   (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) {
                end--;
            }
            if (end == pos) {
                unsigned char c = static_cast<unsigned char>(s[pos]);
                size_t len = 1;
                if ((c & 0x80) == 0) len = 1;
                else if ((c & 0xE0) == 0xC0) len = 2;
                else if ((c & 0xF0) == 0xE0) len = 3;
                else if ((c & 0xF8) == 0xF0) len = 4;
                return std::min(len, remaining);
            }
            return end - pos;
        };

        const size_t maxChunkBytes = 64;
        size_t pos = 0;
        while (pos < outputText_.size()) {
            size_t n = utf8ChunkSize(outputText_, pos, maxChunkBytes);
            if (n == 0) break;
            std::string chunk = outputText_.substr(pos, n);
            pos += n;

            Json::Value deltaEvent(Json::objectValue);
            deltaEvent["type"] = "response.output_text.delta";
            deltaEvent["sequence_number"] = sequenceNumber_++;
            deltaEvent["item_id"] = "msg_" + responseId_;
            deltaEvent["output_index"] = outputItemIndex_;
            deltaEvent["content_index"] = 0;
            deltaEvent["delta"] = chunk;
            sendSseEvent("response.output_text.delta", deltaEvent);
        }
    }
}

void ResponsesSseSink::handleToolCallDone(const generation::ToolCallDone& event) {
    toolCalls_.push_back(event);
}

void ResponsesSseSink::handleCompleted(const generation::Completed& event) {

    Json::Value outputItem;
    outputItem["type"] = "message";
    outputItem["id"] = "msg_" + responseId_;
    outputItem["status"] = "completed";
    outputItem["role"] = "assistant";
    
    Json::Value content(Json::arrayValue);
    // 只有当有文本时才添加文本内容
    if (!outputText_.empty()) {
        Json::Value textContent;
        textContent["type"] = "output_text";
        textContent["text"] = outputText_;
        textContent["annotations"] = Json::Value(Json::arrayValue);
        content.append(textContent);
    }
    outputItem["content"] = content;


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
        outputItem["tool_calls"] = toolCallsJson;
    }
    
    // 发送 response.output_item.done（OpenAI Responses 事件）
    Json::Value outputItemDoneEvent(Json::objectValue);
    outputItemDoneEvent["type"] = "response.output_item.done";
    outputItemDoneEvent["sequence_number"] = sequenceNumber_++;
    outputItemDoneEvent["output_index"] = outputItemIndex_;
    outputItemDoneEvent["item"] = outputItem;
    sendSseEvent("response.output_item.done", outputItemDoneEvent);
    
    // 响应已完成
    Json::Value responseObj = buildResponseObject("completed");
    
    // 添加 usage 信息（如果有）
    if (event.usage.has_value()) {
        Json::Value usage;
        usage["input_tokens"] = event.usage->inputTokens;
        usage["output_tokens"] = event.usage->outputTokens;
        usage["total_tokens"] = event.usage->totalTokens;
        responseObj["usage"] = usage;
    }
    
    // 添加输出内容
    Json::Value output(Json::arrayValue);
    output.append(outputItem);
    responseObj["output"] = output;
    
    Json::Value completedEvent(Json::objectValue);
    completedEvent["type"] = "response.completed";
    completedEvent["sequence_number"] = sequenceNumber_++;
    completedEvent["response"] = responseObj;
    sendSseEvent("response.completed", completedEvent);
}

void ResponsesSseSink::handleError(const generation::Error& event) {
    LOG_ERROR << "[响应SSE] 错误：" << event.message;
    
    Json::Value error;
    error["type"] = generation::errorCodeToString(event.code);
    error["code"] = generation::errorCodeToString(event.code);
    error["message"] = event.message;
    if (!event.detail.empty()) {
        error["detail"] = event.detail;
    }

    Json::Value errorEvent(Json::objectValue);
    errorEvent["type"] = "error";
    errorEvent["sequence_number"] = sequenceNumber_++;
    errorEvent["error"] = error;
    sendSseEvent("error", errorEvent);
}

Json::Value ResponsesSseSink::buildResponseObject(const std::string& status) {
    Json::Value response;
    response["id"] = responseId_;
    response["object"] = "response";
    response["created_at"] = static_cast<Json::Int64>(createdAt_);
    response["status"] = status;
    response["model"] = model_;
    if (status == "completed") {
        response["completed_at"] = static_cast<Json::Int64>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
    } else {
        response["completed_at"] = Json::nullValue;
    }
    response["error"] = Json::nullValue;
    response["metadata"] = Json::Value(Json::objectValue);
    response["output"] = Json::Value(Json::arrayValue);
    response["usage"] = Json::nullValue;
    
    return response;
}
