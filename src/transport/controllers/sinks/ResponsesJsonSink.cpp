#include <transport/controllers/sinks/ResponsesJsonSink.h>
#include <chrono>
#include <sstream>

namespace {
std::string customInput(const generation::ToolCallDone& call)
{
    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(call.arguments);
    if (Json::parseFromStream(builder, input, &parsed, &errors) && parsed.isObject() &&
        parsed.isMember("input") && parsed["input"].isString()) {
        return parsed["input"].asString();
    }
    return call.arguments;
}

Json::Value nativeToolItem(const generation::ToolCallDone& call)
{
    const std::string callId = call.id.empty() ? "call_missing" : call.id;
    Json::Value item(Json::objectValue);
    item["id"] = (call.type == "custom" ? "ctc_" : "fc_") + callId;
    item["call_id"] = callId;
    item["name"] = call.originalName.empty() ? call.name : call.originalName;
    if (!call.namespacePath.empty()) item["namespace"] = call.namespacePath;
    item["status"] = "completed";
    if (call.type == "custom") {
        item["type"] = "custom_tool_call";
        item["input"] = customInput(call);
    } else {
        item["type"] = "function_call";
        item["arguments"] = call.arguments;
    }
    return item;
}
}

ResponsesJsonSink::ResponsesJsonSink(
    ResponseCallback responseCallback,
    const std::string& model,
    int inputTokensEstimated,
    bool nativeToolItems
) : responseCallback_(std::move(responseCallback)),
    model_(model),
    inputTokensEstimated_(inputTokensEstimated),
    nativeToolItems_(nativeToolItems)
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
            if (responseId_.empty()) {
                responseId_ = arg.responseId;
            }
            if (model_.empty() && !arg.model.empty()) {
                model_ = arg.model;
            }
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
            // 会在 已完成 里出现（GenerationService 会把 放进 已完成）
        }
        else if constexpr (std::is_same_v<T, generation::Completed>) {
            if (arg.usage.has_value()) {
                usage_ = *arg.usage;
            }
            if (arg.meta.isObject() && !arg.meta.empty()) {
                meta_ = arg.meta;
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

    Json::Value response = buildResponse();
    if (statusCode_ == 200 && !response.isMember("error") &&
        response.isMember("id") && response["id"].isString()) {
        responseRecord_ = ResponsePersistenceRecord{
            response["id"].asString(), response.toStyledString()};
    }
    if (responseCallback_) {
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
    if (responseId_.empty()) {
        // 按协议约束：Responses 必须包含 id；这里兜底避免返回空字段
        responseId_ = "resp_missing";
    }
    response["id"] = responseId_;
    response["object"] = "response";
    response["created_at"] = static_cast<Json::Int64>(createdAt_);
    response["model"] = model_;
    response["status"] = "completed";

    // 内部字段：供 会话 层存储/续聊映射
    // 响应["_internal_会话_id"] = internal会话Id_;


    Json::Value outputArray(Json::arrayValue);

    Json::Value messageOutput;
    messageOutput["type"] = "message";
    messageOutput["id"] = "msg_" + responseId_;
    messageOutput["status"] = "completed";
    messageOutput["role"] = "assistant";

    Json::Value contentArray(Json::arrayValue);
    if (!collectedText_.empty()) {
        Json::Value textContent;
        textContent["type"] = "output_text";
        textContent["text"] = collectedText_;
        textContent["annotations"] = Json::Value(Json::arrayValue);
        contentArray.append(textContent);
    }
    messageOutput["content"] = contentArray;

    if (nativeToolItems_) {
        for (const auto& tc : toolCalls_) {
            outputArray.append(nativeToolItem(tc));
        }
        if (!collectedText_.empty()) {
            outputArray.append(messageOutput);
        }
    } else {
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
    }
    response["output"] = outputArray;


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
    response["metadata"] = meta_.isObject() ? meta_ : Json::Value(Json::objectValue);
    if (meta_.isObject() && !meta_.empty()) {
        response["_meta"] = meta_;
    }

    return response;
}
