#include <application/generation/protocol/claude/ClaudeJsonSink.h>
#include <application/generation/protocol/claude/ClaudeErrorFormatter.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
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

Json::Value parseToolInput(const std::string& arguments)
{
    if (arguments.empty()) return Json::Value(Json::objectValue);
    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(arguments);
    if (Json::parseFromStream(builder, input, &parsed, &errors) && parsed.isObject()) {
        return parsed;
    }
    return Json::Value(Json::objectValue);
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

ClaudeJsonSink::ClaudeJsonSink(ResponseCallback callback,
                               std::string model,
                               int inputTokensEstimated)
    : callback_(std::move(callback)),
      model_(std::move(model)),
      messageId_(generateMessageId()),
      inputTokensEstimated_(inputTokensEstimated)
{
}

void ClaudeJsonSink::onEvent(const generation::GenerationEvent& event)
{
    if (closed_) return;
    std::visit([this](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, generation::Started>) {
            const auto id = messageIdFromStarted(value.responseId);
            if (!id.empty()) messageId_ = id;
            if (model_.empty() && !value.model.empty()) model_ = value.model;
        } else if constexpr (std::is_same_v<T, generation::OutputTextDelta>) {
            text_ += value.delta;
        } else if constexpr (std::is_same_v<T, generation::OutputTextDone>) {
            if (text_.empty()) text_ = value.text;
        } else if constexpr (std::is_same_v<T, generation::ToolCallDone>) {
            const auto existing = std::find_if(
                toolCalls_.begin(), toolCalls_.end(), [&value](const auto& call) {
                    return !value.id.empty() ? call.id == value.id
                                             : call.id.empty() && call.index == value.index;
                });
            if (existing == toolCalls_.end()) toolCalls_.push_back(value);
            else *existing = value;
            std::stable_sort(toolCalls_.begin(), toolCalls_.end(), [](const auto& left,
                                                                      const auto& right) {
                return left.index < right.index;
            });
        } else if constexpr (std::is_same_v<T, generation::Usage>) {
            usage_ = value;
        } else if constexpr (std::is_same_v<T, generation::Completed>) {
            stopReason_ = stopReason(value.finishReason, !toolCalls_.empty());
            if (value.usage.has_value()) usage_ = value.usage;
        } else if constexpr (std::is_same_v<T, generation::Error>) {
            hasError_ = true;
            errorCode_ = value.code;
            errorMessage_ = value.message;
            statusCode_ = generation::errorCodeToHttpStatus(value.code);
        }
    }, event);
}

void ClaudeJsonSink::onClose()
{
    if (closed_) return;
    closed_ = true;
    if (callback_) callback_(buildResponse(), statusCode_);
}

bool ClaudeJsonSink::isValid() const
{
    return !closed_;
}

Json::Value ClaudeJsonSink::buildResponse() const
{
    if (hasError_) return formatError(errorCode_, errorMessage_);

    Json::Value response(Json::objectValue);
    response["id"] = messageId_.empty() ? generateMessageId() : messageId_;
    response["type"] = "message";
    response["role"] = "assistant";
    response["model"] = model_;
    response["content"] = Json::Value(Json::arrayValue);

    if (!text_.empty()) {
        Json::Value block(Json::objectValue);
        block["type"] = "text";
        block["text"] = text_;
        response["content"].append(std::move(block));
    }
    for (const auto& call : toolCalls_) {
        Json::Value block(Json::objectValue);
        block["type"] = "tool_use";
        block["id"] = call.id;
        block["name"] = call.originalName.empty() ? call.name : call.originalName;
        block["input"] = parseToolInput(call.arguments);
        response["content"].append(std::move(block));
    }

    response["stop_reason"] = stopReason_;
    response["stop_sequence"] = Json::nullValue;
    Json::Value usage(Json::objectValue);
    usage["input_tokens"] = usage_.has_value()
        ? usage_->inputTokens : inputTokensEstimated_;
    usage["output_tokens"] = usage_.has_value()
        ? usage_->outputTokens : static_cast<int>(text_.size() / 4);
    response["usage"] = std::move(usage);
    return response;
}

std::string ClaudeJsonSink::generateMessageId()
{
    return generateMessageIdValue();
}

}  // namespace generation::protocol::claude
