#include "ResponsesSseSink.h"
#include <json/json.h>
#include <chrono>

using namespace drogon;

ResponsesSseSink::ResponsesSseSink(
    StreamCallback streamCallback,
    CloseCallback closeCallback,
    const std::string& responseId,
    const std::string& model
) : streamCallback_(std::move(streamCallback)),
    closeCallback_(std::move(closeCallback)),
    responseId_(responseId),
    model_(model)
{
    LOG_DEBUG << "[响应SSE] 已创建, 响应ID: " << responseId_ << ", 模型: " << model_;
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
        else if constexpr (std::is_same_v<T, generation::Usage>) {
            LOG_DEBUG << "[响应SSE] 令牌用量: 输入=" << arg.inputTokens 
                     << ", 输出=" << arg.outputTokens;
            // Usage 信息会在 Completed 事件中包含
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
        }
    }
}

void ResponsesSseSink::handleStarted(const generation::Started& event) {
    LOG_DEBUG << "[响应SSE] 开始事件, 响应ID: " << event.responseId;
    
    // 1. response.created
    Json::Value responseObj = buildResponseObject("in_progress");
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    sendSseEvent("response.created", Json::writeString(writer, responseObj));
    
    // 2. response.in_progress (same response object)
    sendSseEvent("response.in_progress", Json::writeString(writer, responseObj));
    
    // 3. response.output_item.added
    Json::Value outputItem;
    outputItem["type"] = "message";
    outputItem["id"] = "msg_" + responseId_;
    outputItem["status"] = "in_progress";
    outputItem["role"] = "assistant";
    outputItem["content"] = Json::Value(Json::arrayValue);
    
    Json::Value outputItemEvent;
    outputItemEvent["response_id"] = responseId_;
    outputItemEvent["output_index"] = outputItemIndex_;
    outputItemEvent["item"] = outputItem;
    sendSseEvent("response.output_item.added", Json::writeString(writer, outputItemEvent));
    
    // 4. response.content_part.added
    Json::Value contentPart;
    contentPart["type"] = "output_text";
    contentPart["text"] = "";
    
    Json::Value contentPartEvent;
    contentPartEvent["response_id"] = responseId_;
    contentPartEvent["item_id"] = "msg_" + responseId_;
    contentPartEvent["output_index"] = outputItemIndex_;
    contentPartEvent["content_index"] = 0;
    contentPartEvent["part"] = contentPart;
    sendSseEvent("response.content_part.added", Json::writeString(writer, contentPartEvent));
}

void ResponsesSseSink::handleOutputTextDelta(const generation::OutputTextDelta& event) {
    // 累积文本
    outputText_ += event.delta;
    
    // response.output_text.delta
    Json::Value deltaEvent;
    deltaEvent["response_id"] = responseId_;
    deltaEvent["item_id"] = "msg_" + responseId_;
    deltaEvent["output_index"] = outputItemIndex_;
    deltaEvent["content_index"] = 0;
    deltaEvent["delta"] = event.delta;
    
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    sendSseEvent("response.output_text.delta", Json::writeString(writer, deltaEvent));
}

void ResponsesSseSink::handleOutputTextDone(const generation::OutputTextDone& event) {
    // 如果之前没有通过 delta 发送，使用完整文本
    if (outputText_.empty()) {
        outputText_ = event.text;
    }
    
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    
    // 1. response.output_text.done
    Json::Value textDoneEvent;
    textDoneEvent["response_id"] = responseId_;
    textDoneEvent["item_id"] = "msg_" + responseId_;
    textDoneEvent["output_index"] = outputItemIndex_;
    textDoneEvent["content_index"] = 0;
    textDoneEvent["text"] = outputText_;
    sendSseEvent("response.output_text.done", Json::writeString(writer, textDoneEvent));
    
    // 2. response.content_part.done
    Json::Value contentPart;
    contentPart["type"] = "output_text";
    contentPart["text"] = outputText_;
    
    Json::Value contentPartDoneEvent;
    contentPartDoneEvent["response_id"] = responseId_;
    contentPartDoneEvent["item_id"] = "msg_" + responseId_;
    contentPartDoneEvent["output_index"] = outputItemIndex_;
    contentPartDoneEvent["content_index"] = 0;
    contentPartDoneEvent["part"] = contentPart;
    sendSseEvent("response.content_part.done", Json::writeString(writer, contentPartDoneEvent));
    
    // 3. response.output_item.done
    Json::Value outputItem;
    outputItem["type"] = "message";
    outputItem["id"] = "msg_" + responseId_;
    outputItem["status"] = "completed";
    outputItem["role"] = "assistant";
    
    Json::Value content(Json::arrayValue);
    Json::Value textContent;
    textContent["type"] = "output_text";
    textContent["text"] = outputText_;
    content.append(textContent);
    outputItem["content"] = content;
    
    Json::Value outputItemDoneEvent;
    outputItemDoneEvent["response_id"] = responseId_;
    outputItemDoneEvent["output_index"] = outputItemIndex_;
    outputItemDoneEvent["item"] = outputItem;
    sendSseEvent("response.output_item.done", Json::writeString(writer, outputItemDoneEvent));
}

void ResponsesSseSink::handleCompleted(const generation::Completed& event) {
    // response.completed
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
    Json::Value outputItem;
    outputItem["type"] = "message";
    outputItem["id"] = "msg_" + responseId_;
    outputItem["status"] = "completed";
    outputItem["role"] = "assistant";
    
    Json::Value content(Json::arrayValue);
    Json::Value textContent;
    textContent["type"] = "output_text";
    textContent["text"] = outputText_;
    content.append(textContent);
    outputItem["content"] = content;
    output.append(outputItem);
    responseObj["output"] = output;
    
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    sendSseEvent("response.completed", Json::writeString(writer, responseObj));
}

void ResponsesSseSink::handleError(const generation::Error& event) {
    LOG_ERROR << "[响应SSE] 错误: " << event.message;
    
    // response.failed
    Json::Value responseObj = buildResponseObject("failed");
    
    Json::Value error;
    error["type"] = generation::errorCodeToString(event.code);
    error["code"] = generation::errorCodeToString(event.code);
    error["message"] = event.message;
    if (!event.detail.empty()) {
        error["detail"] = event.detail;
    }
    responseObj["error"] = error;
    
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    sendSseEvent("response.failed", Json::writeString(writer, responseObj));
}

Json::Value ResponsesSseSink::buildResponseObject(const std::string& status) {
    Json::Value response;
    response["id"] = responseId_;
    response["object"] = "response";
    response["created_at"] = static_cast<Json::Int64>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    response["status"] = status;
    response["model"] = model_;
    response["output"] = Json::Value(Json::arrayValue);
    
    return response;
}
