#include "ResponsesSseSink.h"
#include <json/json.h>
#include <chrono>
#include <algorithm>
#include <sstream>

using namespace drogon;

ResponsesSseSink::ResponsesSseSink(
    StreamCallback streamCallback,
    CloseCallback closeCallback,
    const std::string& model,
    bool nativeToolItems
) : streamCallback_(std::move(streamCallback)),
    closeCallback_(std::move(closeCallback)),
    model_(model),
    nativeToolItems_(nativeToolItems)
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

Json::Value ResponsesSseSink::buildTextOutputItem(const std::string& status) const {
    Json::Value item(Json::objectValue);
    item["type"] = "message";
    item["id"] = "msg_" + responseId_;
    item["status"] = status;
    item["role"] = "assistant";
    Json::Value content(Json::arrayValue);
    if (status == "completed" && !outputText_.empty()) {
        Json::Value text(Json::objectValue);
        text["type"] = "output_text";
        text["text"] = outputText_;
        text["annotations"] = Json::Value(Json::arrayValue);
        content.append(text);
    }
    item["content"] = content;
    return item;
}

void ResponsesSseSink::ensureTextItemAdded() {
    if (textItemAdded_) return;
    textItemAdded_ = true;
    textOutputIndex_ = nativeToolItems_
        ? static_cast<int>(nativeOutputItems_.size())
        : outputItemIndex_;
    Json::Value event(Json::objectValue);
    event["type"] = "response.output_item.added";
    event["sequence_number"] = sequenceNumber_++;
    event["output_index"] = textOutputIndex_;
    event["item"] = buildTextOutputItem("in_progress");
    sendSseEvent("response.output_item.added", event);
}

std::string ResponsesSseSink::customToolInput(const generation::ToolCallDone& event) {
    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(event.arguments);
    if (Json::parseFromStream(builder, input, &parsed, &errors) && parsed.isObject() &&
        parsed.isMember("input") && parsed["input"].isString()) {
        return parsed["input"].asString();
    }
    return event.arguments;
}

void ResponsesSseSink::emitNativeToolCall(const generation::ToolCallDone& event) {
    const bool custom = event.type == "custom";
    const std::string callId = event.id.empty()
        ? ("call_" + responseId_ + "_" + std::to_string(nativeOutputItems_.size()))
        : event.id;
    const std::string itemId = (custom ? "ctc_" : "fc_") + callId;
    const int index = static_cast<int>(nativeOutputItems_.size());
    const std::string payload = custom ? customToolInput(event) : event.arguments;

    Json::Value addedItem(Json::objectValue);
    addedItem["type"] = custom ? "custom_tool_call" : "function_call";
    addedItem["id"] = itemId;
    addedItem["call_id"] = callId;
    addedItem["name"] = event.originalName.empty() ? event.name : event.originalName;
    if (!event.namespacePath.empty()) addedItem["namespace"] = event.namespacePath;
    addedItem[custom ? "input" : "arguments"] = "";
    addedItem["status"] = "in_progress";

    Json::Value added(Json::objectValue);
    added["type"] = "response.output_item.added";
    added["sequence_number"] = sequenceNumber_++;
    added["output_index"] = index;
    added["item"] = addedItem;
    sendSseEvent("response.output_item.added", added);

    const std::string deltaType = custom
        ? "response.custom_tool_call_input.delta"
        : "response.function_call_arguments.delta";
    Json::Value delta(Json::objectValue);
    delta["type"] = deltaType;
    delta["sequence_number"] = sequenceNumber_++;
    delta["item_id"] = itemId;
    delta["output_index"] = index;
    delta["delta"] = payload;
    sendSseEvent(deltaType, delta);

    const std::string doneType = custom
        ? "response.custom_tool_call_input.done"
        : "response.function_call_arguments.done";
    Json::Value done(Json::objectValue);
    done["type"] = doneType;
    done["sequence_number"] = sequenceNumber_++;
    done["item_id"] = itemId;
    done["output_index"] = index;
    done[custom ? "input" : "arguments"] = payload;
    sendSseEvent(doneType, done);

    Json::Value finalItem = addedItem;
    finalItem[custom ? "input" : "arguments"] = payload;
    finalItem["status"] = "completed";
    nativeOutputItems_.append(finalItem);

    Json::Value itemDone(Json::objectValue);
    itemDone["type"] = "response.output_item.done";
    itemDone["sequence_number"] = sequenceNumber_++;
    itemDone["output_index"] = index;
    itemDone["item"] = finalItem;
    sendSseEvent("response.output_item.done", itemDone);
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

    if (nativeToolItems_) {
        return;
    }
    textItemAdded_ = true;
    textOutputIndex_ = outputItemIndex_;

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
    ensureTextItemAdded();
    sawDelta_ = true;
    outputText_ += event.delta;

    Json::Value deltaEvent(Json::objectValue);
    deltaEvent["type"] = "response.output_text.delta";
    deltaEvent["sequence_number"] = sequenceNumber_++;
    deltaEvent["item_id"] = "msg_" + responseId_;
    deltaEvent["output_index"] = textOutputIndex_;
    deltaEvent["content_index"] = 0;
    deltaEvent["delta"] = event.delta;
    
    sendSseEvent("response.output_text.delta", deltaEvent);
}

void ResponsesSseSink::handleOutputTextDone(const generation::OutputTextDone& event) {
    // 如果之前没有通过 发送，使用完整文本
    if (outputText_.empty()) {
        outputText_ = event.text;
    }
    if (!outputText_.empty()) {
        ensureTextItemAdded();
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
            deltaEvent["output_index"] = textOutputIndex_;
            deltaEvent["content_index"] = 0;
            deltaEvent["delta"] = chunk;
            sendSseEvent("response.output_text.delta", deltaEvent);
        }
    }
}

void ResponsesSseSink::handleToolCallDone(const generation::ToolCallDone& event) {
    toolCalls_.push_back(event);
    if (nativeToolItems_) {
        emitNativeToolCall(event);
    }
}

void ResponsesSseSink::handleCompleted(const generation::Completed& event) {
    if (event.meta.isObject() && !event.meta.empty()) {
        meta_ = event.meta;
    }

    auto addUsage = [&event](Json::Value& responseObj) {
        if (!event.usage.has_value()) return;
        Json::Value usage(Json::objectValue);
        usage["input_tokens"] = event.usage->inputTokens;
        usage["output_tokens"] = event.usage->outputTokens;
        usage["total_tokens"] = event.usage->totalTokens;
        responseObj["usage"] = usage;
    };

    if (nativeToolItems_) {
        if (textItemAdded_) {
            Json::Value textDone(Json::objectValue);
            textDone["type"] = "response.output_text.done";
            textDone["sequence_number"] = sequenceNumber_++;
            textDone["item_id"] = "msg_" + responseId_;
            textDone["output_index"] = textOutputIndex_;
            textDone["content_index"] = 0;
            textDone["text"] = outputText_;
            sendSseEvent("response.output_text.done", textDone);

            Json::Value textItem = buildTextOutputItem("completed");
            Json::Value itemDone(Json::objectValue);
            itemDone["type"] = "response.output_item.done";
            itemDone["sequence_number"] = sequenceNumber_++;
            itemDone["output_index"] = textOutputIndex_;
            itemDone["item"] = textItem;
            sendSseEvent("response.output_item.done", itemDone);
            nativeOutputItems_.append(textItem);
        }

        Json::Value responseObj = buildResponseObject("completed");
        addUsage(responseObj);
        responseObj["output"] = nativeOutputItems_;

        Json::Value completedEvent(Json::objectValue);
        completedEvent["type"] = "response.completed";
        completedEvent["sequence_number"] = sequenceNumber_++;
        completedEvent["response"] = responseObj;
        sendSseEvent("response.completed", completedEvent);
        return;
    }

    Json::Value outputItem = buildTextOutputItem("completed");
    if (!toolCalls_.empty()) {
        Json::Value toolCallsJson(Json::arrayValue);
        for (const auto& tc : toolCalls_) {
            Json::Value call(Json::objectValue);
            call["id"] = tc.id;
            call["type"] = "function";
            Json::Value func(Json::objectValue);
            func["name"] = tc.name;
            func["arguments"] = tc.arguments;
            call["function"] = func;
            toolCallsJson.append(call);
        }
        outputItem["tool_calls"] = toolCallsJson;
    }

    Json::Value outputItemDoneEvent(Json::objectValue);
    outputItemDoneEvent["type"] = "response.output_item.done";
    outputItemDoneEvent["sequence_number"] = sequenceNumber_++;
    outputItemDoneEvent["output_index"] = outputItemIndex_;
    outputItemDoneEvent["item"] = outputItem;
    sendSseEvent("response.output_item.done", outputItemDoneEvent);

    Json::Value responseObj = buildResponseObject("completed");
    addUsage(responseObj);
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
    LOG_ERROR << "[响应SSE] 错误: code=" << generation::errorCodeToString(event.code)
              << ", messagePresent=" << !event.message.empty()
              << ", messageSize=" << event.message.size()
              << ", detailPresent=" << !event.detail.empty()
              << ", detailSize=" << event.detail.size();
    
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
    response["metadata"] = meta_.isObject() ? meta_ : Json::Value(Json::objectValue);
    if (meta_.isObject() && !meta_.empty()) {
        response["_meta"] = meta_;
    }
    response["output"] = Json::Value(Json::arrayValue);
    response["usage"] = Json::nullValue;
    
    return response;
}
