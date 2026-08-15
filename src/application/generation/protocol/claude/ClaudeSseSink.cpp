#include <application/generation/protocol/claude/ClaudeSseSink.h>
#include <application/generation/protocol/claude/ClaudeErrorFormatter.h>

#include <chrono>
#include <random>
#include <utility>

namespace generation::protocol::claude {
namespace {

std::string messageIdFromStarted(const std::string& responseId)
{
    if (responseId.empty()) return {};
    return responseId.rfind("msg_", 0) == 0 ? responseId : "msg_" + responseId;
}

std::string stopReason(const std::string& finishReason, bool hasTools)
{
    if (hasTools || finishReason == "tool_calls") return "tool_use";
    if (finishReason == "length" || finishReason == "max_tokens") return "max_tokens";
    if (finishReason == "stop_sequence") return "stop_sequence";
    if (finishReason == "pause_turn") return "pause_turn";
    if (finishReason == "refusal") return "refusal";
    return "end_turn";
}

std::string generateMessageIdValue()
{
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::random_device source;
    std::mt19937 generator(source());
    std::uniform_int_distribution<> suffix(1000, 9999);
    return "msg_" + std::to_string(timestamp) + std::to_string(suffix(generator));
}

}  // namespace

ClaudeSseSink::ClaudeSseSink(StreamCallback streamCallback,
                             CloseCallback closeCallback,
                             std::string model,
                             int inputTokensEstimated)
    : streamCallback_(std::move(streamCallback)),
      closeCallback_(std::move(closeCallback)),
      model_(std::move(model)),
      messageId_(generateMessageId()),
      inputTokensEstimated_(inputTokensEstimated)
{
}

void ClaudeSseSink::onEvent(const generation::GenerationEvent& event)
{
    if (closed_) return;
    std::visit([this](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, generation::Started>) {
            ensureMessageStarted(&value);
        } else if constexpr (std::is_same_v<T, generation::OutputTextDelta>) {
            ensureMessageStarted();
            ensureTextBlock();
            Json::Value eventBody(Json::objectValue);
            eventBody["type"] = "content_block_delta";
            eventBody["index"] = textIndex_;
            eventBody["delta"]["type"] = "text_delta";
            eventBody["delta"]["text"] = value.delta;
            sendEvent("content_block_delta", eventBody);
            text_ += value.delta;
            outputTokensEstimated_ = static_cast<int>(text_.size() / 4);
        } else if constexpr (std::is_same_v<T, generation::OutputTextDone>) {
            ensureMessageStarted();
            ensureTextBlock();
            if (text_.empty() && !value.text.empty()) {
                Json::Value eventBody(Json::objectValue);
                eventBody["type"] = "content_block_delta";
                eventBody["index"] = textIndex_;
                eventBody["delta"]["type"] = "text_delta";
                eventBody["delta"]["text"] = value.text;
                sendEvent("content_block_delta", eventBody);
                text_ = value.text;
                outputTokensEstimated_ = static_cast<int>(text_.size() / 4);
            }
            closeTextBlock();
        } else if constexpr (std::is_same_v<T, generation::ToolCallStarted>) {
            ensureMessageStarted();
            const auto key = toolStateKey(value.id, value.index);
            if (startedTools_.insert(key).second) {
                const int index = nextContentIndex_++;
                toolIndexes_[key] = index;
                Json::Value eventBody(Json::objectValue);
                eventBody["type"] = "content_block_start";
                eventBody["index"] = index;
                eventBody["content_block"]["type"] = "tool_use";
                eventBody["content_block"]["id"] = value.id;
                eventBody["content_block"]["name"] =
                    value.originalName.empty() ? value.name : value.originalName;
                eventBody["content_block"]["input"] = Json::Value(Json::objectValue);
                sendEvent("content_block_start", eventBody);
            }
        } else if constexpr (std::is_same_v<T, generation::ToolArgumentsDelta>) {
            ensureMessageStarted();
            const auto key = toolStateKey(value.id, value.index);
            if (completedTools_.count(key) != 0 ||
                !toolSequences_[key].insert(value.sequence).second) {
                return;
            }
            const auto index = toolIndexes_.find(key);
            if (index == toolIndexes_.end()) return;
            Json::Value eventBody(Json::objectValue);
            eventBody["type"] = "content_block_delta";
            eventBody["index"] = index->second;
            eventBody["delta"]["type"] = "input_json_delta";
            eventBody["delta"]["partial_json"] = value.delta;
            sendEvent("content_block_delta", eventBody);
        } else if constexpr (std::is_same_v<T, generation::ToolCallDone>) {
            ensureMessageStarted();
            const auto key = toolStateKey(value.id, value.index);
            if (startedTools_.count(key) == 0) {
                generation::ToolCallStarted started;
                started.id = value.id;
                started.name = value.name;
                started.index = value.index;
                started.originalName = value.originalName;
                onEvent(started);
                generation::ToolArgumentsDelta delta;
                delta.id = value.id;
                delta.index = value.index;
                delta.delta = value.arguments;
                delta.sequence = 0;
                onEvent(delta);
            }
            closeToolBlock(key);
        } else if constexpr (std::is_same_v<T, generation::Usage>) {
            usage_ = value;
        } else if constexpr (std::is_same_v<T, generation::Completed>) {
            if (value.usage.has_value()) usage_ = value.usage;
            finishMessage(value);
        } else if constexpr (std::is_same_v<T, generation::Error>) {
            sendError(value);
        }
    }, event);
}

void ClaudeSseSink::onClose()
{
    if (closed_) return;
    closed_ = true;
    if (closeCallback_) closeCallback_();
}

bool ClaudeSseSink::isValid() const
{
    return !closed_;
}

void ClaudeSseSink::sendEvent(const std::string& type, const Json::Value& data)
{
    if (closed_ || !streamCallback_) return;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    writer["emitUTF8"] = true;
    if (!streamCallback_("event: " + type + "\ndata: " +
                         Json::writeString(writer, data) + "\n\n")) {
        closed_ = true;
        if (closeCallback_) closeCallback_();
    }
}

void ClaudeSseSink::sendError(const generation::Error& error)
{
    if (messageFinished_) return;
    sendEvent("error", formatError(error.code, error.message));
    messageFinished_ = true;
}

void ClaudeSseSink::ensureMessageStarted(const generation::Started* started)
{
    if (messageStarted_) return;
    if (started) {
        const auto id = messageIdFromStarted(started->responseId);
        if (!id.empty()) messageId_ = id;
        if (model_.empty() && !started->model.empty()) model_ = started->model;
    }
    messageStarted_ = true;
    Json::Value eventBody(Json::objectValue);
    eventBody["type"] = "message_start";
    auto& message = eventBody["message"];
    message["id"] = messageId_;
    message["type"] = "message";
    message["role"] = "assistant";
    message["model"] = model_;
    message["content"] = Json::Value(Json::arrayValue);
    message["stop_reason"] = Json::nullValue;
    message["stop_sequence"] = Json::nullValue;
    message["usage"]["input_tokens"] = inputTokensEstimated_;
    message["usage"]["output_tokens"] = 0;
    sendEvent("message_start", eventBody);
}

void ClaudeSseSink::ensureTextBlock()
{
    if (textBlockOpen_) return;
    textBlockOpen_ = true;
    textIndex_ = nextContentIndex_++;
    Json::Value eventBody(Json::objectValue);
    eventBody["type"] = "content_block_start";
    eventBody["index"] = textIndex_;
    eventBody["content_block"]["type"] = "text";
    eventBody["content_block"]["text"] = "";
    sendEvent("content_block_start", eventBody);
}

void ClaudeSseSink::closeTextBlock()
{
    if (!textBlockOpen_) return;
    Json::Value eventBody(Json::objectValue);
    eventBody["type"] = "content_block_stop";
    eventBody["index"] = textIndex_;
    sendEvent("content_block_stop", eventBody);
    textBlockOpen_ = false;
}

void ClaudeSseSink::closeToolBlock(const std::string& key)
{
    if (!completedTools_.insert(key).second) return;
    const auto index = toolIndexes_.find(key);
    if (index == toolIndexes_.end()) return;
    Json::Value eventBody(Json::objectValue);
    eventBody["type"] = "content_block_stop";
    eventBody["index"] = index->second;
    sendEvent("content_block_stop", eventBody);
}

void ClaudeSseSink::finishMessage(const generation::Completed& completed)
{
    if (messageFinished_) return;
    ensureMessageStarted();
    closeTextBlock();
    for (const auto& entry : toolIndexes_) closeToolBlock(entry.first);

    Json::Value delta(Json::objectValue);
    delta["type"] = "message_delta";
    delta["delta"]["stop_reason"] = stopReason(
        completed.finishReason, !toolIndexes_.empty());
    delta["delta"]["stop_sequence"] = Json::nullValue;
    delta["usage"]["output_tokens"] = usage_.has_value()
        ? usage_->outputTokens : outputTokensEstimated_;
    sendEvent("message_delta", delta);

    Json::Value stop(Json::objectValue);
    stop["type"] = "message_stop";
    sendEvent("message_stop", stop);
    messageFinished_ = true;
}

std::string ClaudeSseSink::toolStateKey(const std::string& id, int index)
{
    return id.empty() ? "index:" + std::to_string(index) : "id:" + id;
}

std::string ClaudeSseSink::generateMessageId()
{
    return generateMessageIdValue();
}

}  // namespace generation::protocol::claude
