#include <application/generation/protocol/claude/ClaudeRequestAdapter.h>

#include <platform/Uuid.h>

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace generation::protocol::claude {
namespace {

std::string requestId(const aiapi::RequestHeaders& headers)
{
    if (!headers.requestId.empty()) return headers.requestId;
    if (!headers.correlationId.empty()) return headers.correlationId;
    return "req_" + platform::generateUuidV4();
}

Json::Value clientInfo(const aiapi::RequestHeaders& headers)
{
    Json::Value result(Json::objectValue);
    result["client_type"] = "ClaudeCode";
    if (!headers.userAgent.empty()) result["raw_user_agent"] = headers.userAgent;
    if (!headers.authorization.empty()) {
        std::string authorization = headers.authorization;
        if (authorization.rfind("Bearer ", 0) == 0 ||
            authorization.rfind("bearer ", 0) == 0) {
            authorization.erase(0, 7);
        }
        result["client_authorization"] = authorization;
    }
    return result;
}

std::string textFromContent(const Json::Value& content)
{
    if (content.isString()) return content.asString();
    if (!content.isArray()) return {};

    std::string result;
    for (const auto& block : content) {
        if (!block.isObject()) continue;
        const auto type = block.get("type", "").asString();
        if (type == "text") {
            result += block.get("text", "").asString();
        } else if (type == "thinking") {
            result += block.get("thinking", block.get("text", "")).asString();
        } else if (type == "tool_result") {
            result += textFromContent(block.get("content", ""));
        }
    }
    return result;
}

bool imageFromBlock(const Json::Value& block, ImageInfo& image)
{
    if (!block.isObject()) return false;
    const auto& source = block["source"];
    if (!source.isObject()) return false;
    const auto type = source.get("type", "").asString();
    if (type == "base64" && source.get("data", "").isString()) {
        image.base64Data = source["data"].asString();
        image.mediaType = source.get("media_type", "image/png").asString();
        return !image.base64Data.empty();
    }
    if (type == "url" && source.get("url", "").isString()) {
        image.uploadedUrl = source["url"].asString();
        image.mediaType = source.get("media_type", "").asString();
        return !image.uploadedUrl.empty();
    }
    return false;
}

void appendTextBlock(Message& message, const std::string& text)
{
    if (text.empty()) return;
    if (!message.blocks.empty() && message.blocks.back().type == ContentBlockType::Text) {
        message.blocks.back().text += text;
        return;
    }
    ContentBlock block;
    block.type = ContentBlockType::Text;
    block.text = text;
    message.blocks.push_back(std::move(block));
}

bool parseSystemPromptContent(const Json::Value& content,
                              std::string& output,
                              std::string& error)
{
    output.clear();
    if (content.isString()) {
        output = content.asString();
        return true;
    }
    if (!content.isArray()) {
        error = "system content must be a string or array of text blocks";
        return false;
    }

    for (const auto& block : content) {
        if (!block.isObject() || block.get("type", "").asString() != "text" ||
            !block.get("text", "").isString()) {
            error = "system content supports text blocks only";
            return false;
        }
        if (!output.empty()) output += "\n";
        output += block["text"].asString();
    }
    return true;
}

void appendSystemPrompt(std::string& target, const std::string& addition)
{
    if (addition.empty()) return;
    if (!target.empty()) target += "\n";
    target += addition;
}

bool appendContentBlocks(const Json::Value& content,
                         Message& message,
                         std::vector<ImageInfo>* images,
                         std::string& error)
{
    if (content.isString()) {
        appendTextBlock(message, content.asString());
        return true;
    }
    if (!content.isArray()) {
        error = "message content must be a string or array";
        return false;
    }

    for (const auto& blockValue : content) {
        if (!blockValue.isObject()) {
            error = "message content blocks must be objects";
            return false;
        }
        const auto type = blockValue.get("type", "").asString();
        if (type == "text") {
            appendTextBlock(message, blockValue.get("text", "").asString());
            continue;
        }
        if (type == "thinking") {
            ContentBlock block;
            block.type = ContentBlockType::Thinking;
            block.text = blockValue.get("thinking", blockValue.get("text", "")).asString();
            message.blocks.push_back(std::move(block));
            continue;
        }
        if (type == "redacted_thinking") {
            // The canonical model has no encrypted thinking payload. Preserve
            // message ordering without exposing or fabricating its contents.
            continue;
        }
        if (type == "image") {
            ImageInfo image;
            if (!imageFromBlock(blockValue, image)) {
                error = "image content block has an invalid source";
                return false;
            }
            if (images) images->push_back(std::move(image));
            continue;
        }
        if (type == "tool_use") {
            const auto id = blockValue.get("id", "").asString();
            const auto name = blockValue.get("name", "").asString();
            if (id.empty() || name.empty()) {
                error = "tool_use content block requires id and name";
                return false;
            }
            ContentBlock block;
            block.type = ContentBlockType::ToolUse;
            block.toolCallId = id;
            block.toolName = name;
            block.toolInput = blockValue.get("input", Json::Value(Json::objectValue));
            message.blocks.push_back(std::move(block));
            continue;
        }
        if (type == "tool_result") {
            const auto id = blockValue.get("tool_use_id", "").asString();
            if (id.empty()) {
                error = "tool_result content block requires tool_use_id";
                return false;
            }
            ContentBlock block;
            block.type = ContentBlockType::ToolResult;
            block.toolCallId = id;
            block.toolResult = textFromContent(blockValue.get("content", ""));
            block.toolResultIsError = blockValue.get("is_error", false).asBool();
            message.blocks.push_back(std::move(block));
            continue;
        }

        error = "unsupported Claude content block type: " + type;
        return false;
    }
    return true;
}

std::string toolResultText(const ContentBlock& block)
{
    std::string result = "\n[tool_result tool_use_id=" + block.toolCallId;
    if (block.toolResultIsError) result += " is_error=true";
    result += "]\n" + block.toolResult + "\n[/tool_result]\n";
    return result;
}

std::string currentText(const Message& message)
{
    std::string result;
    for (const auto& block : message.blocks) {
        if (block.type == ContentBlockType::Text ||
            block.type == ContentBlockType::Thinking) {
            result += block.text;
        }
    }
    return result;
}

bool isAuxiliaryClaudeCodePrompt(const std::string& text,
                                 const std::string& userAgent)
{
    std::string loweredUserAgent = userAgent;
    std::transform(loweredUserAgent.begin(), loweredUserAgent.end(),
                   loweredUserAgent.begin(), [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (loweredUserAgent.find("claude-code") == std::string::npos) return false;

    // Claude Code sends a few client-side requests through the same Messages
    // endpoint (title generation, next-prompt suggestion, and recap).  They
    // may carry the full user-text prefix, but they are not a new durable
    // conversation turn and therefore must not advance the replay snapshot.
    return text.find("[SUGGESTION MODE:") != std::string::npos ||
        text.find("The user stepped away and is coming back.") != std::string::npos ||
        (text.find("<session>") != std::string::npos &&
         text.find("Write the title in the predominant language") != std::string::npos);
}

void appendCurrentInputPart(GenerationRequest& request,
                            std::string text,
                            bool isToolResult = false,
                            std::string toolResultCallId = {},
                            bool isReplayableText = false,
                            bool isAuxiliary = false)
{
    if (text.empty()) return;
    request.currentInput += text;
    CurrentInputPart part;
    part.text = std::move(text);
    part.isToolResult = isToolResult;
    part.toolResultCallId = std::move(toolResultCallId);
    part.isReplayableText = isReplayableText;
    part.isAuxiliary = isAuxiliary;
    request.currentInputParts.push_back(std::move(part));
}

void appendCurrentInputMessage(GenerationRequest& request,
                               const Message& message,
                               const std::string& userAgent)
{
    const std::string text = currentText(message);
    appendCurrentInputPart(request, text, false, {}, true,
                           isAuxiliaryClaudeCodePrompt(text, userAgent));

    bool hasPriorToolResult = false;
    for (const auto& block : message.blocks) {
        if (block.type != ContentBlockType::ToolResult) continue;

        std::string rendered;
        if (hasPriorToolResult) rendered += "\n";
        rendered += toolResultText(block);
        appendCurrentInputPart(request, std::move(rendered), true, block.toolCallId);
        hasPriorToolResult = true;
    }
    // Preserve the historical adapter's per-message separator as an opaque
    // non-tool fragment.  The core may later drop a stale tool-result fragment
    // without touching surrounding current-turn text.
    appendCurrentInputPart(request, "\n");
}

void appendHistoryMessages(const Message& source, std::vector<Message>& output)
{
    if (source.role != MessageRole::User || source.toolResultCallId().empty()) {
        output.push_back(source);
        return;
    }

    Message user;
    user.role = MessageRole::User;
    const auto flushUser = [&output, &user]() {
        if (user.blocks.empty()) return;
        output.push_back(std::move(user));
        user = Message{};
        user.role = MessageRole::User;
    };

    for (const auto& block : source.blocks) {
        if (block.type != ContentBlockType::ToolResult) {
            user.blocks.push_back(block);
            continue;
        }
        flushUser();
        Message tool;
        tool.role = MessageRole::Tool;
        tool.blocks.push_back(block);
        output.push_back(std::move(tool));
    }
    flushUser();
}

void appendSafeMetadata(const Json::Value& metadata, Json::Value& extensions)
{
    if (!metadata.isObject()) return;
    for (const auto& name : metadata.getMemberNames()) {
        std::string lowered = name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lowered.find("token") != std::string::npos ||
            lowered.find("secret") != std::string::npos ||
            lowered.find("password") != std::string::npos ||
            lowered.find("authorization") != std::string::npos ||
            lowered.find("api_key") != std::string::npos) {
            continue;
        }
        extensions[name] = metadata[name];
    }
}

AdapterResult invalid(std::string message)
{
    return AdapterResult{{}, platform::Error::badRequest(std::move(message))};
}

}  // namespace

AdapterResult ClaudeRequestAdapter::adapt(const RawProtocolRequest& raw) const
{
    if (!raw.body.isObject()) return invalid("Request body must be a JSON object");

    const auto model = raw.body.get("model", "").asString();
    if (model.empty()) return invalid("model is required");
    if (!raw.body.isMember("max_tokens") || !raw.body["max_tokens"].isIntegral() ||
        raw.body["max_tokens"].asInt() <= 0) {
        return invalid("max_tokens must be a positive integer");
    }
    if (!raw.body.isMember("messages") || !raw.body["messages"].isArray() ||
        raw.body["messages"].empty()) {
        return invalid("messages must be a non-empty array");
    }

    GenerationRequest request;
    request.responseLifecycle = ResponseLifecycle::Immediate;
    request.requestId = requestId(raw.headers);
    request.model = model;
    if (raw.body.isMember("stream") && !raw.body["stream"].isBool()) {
        return invalid("stream must be boolean");
    }
    request.stream = raw.body.get("stream", false).asBool();
    request.maxOutputTokens = raw.body["max_tokens"].asInt();
    request.clientInfo = clientInfo(raw.headers);

    if (raw.body.isMember("temperature")) {
        if (!raw.body["temperature"].isNumeric()) return invalid("temperature must be numeric");
        if (raw.body["temperature"].asDouble() < 0.0 ||
            raw.body["temperature"].asDouble() > 1.0) {
            return invalid("temperature must be between 0 and 1");
        }
        request.temperature = raw.body["temperature"].asDouble();
    }
    if (raw.body.isMember("top_p")) {
        if (!raw.body["top_p"].isNumeric()) return invalid("top_p must be numeric");
        if (raw.body["top_p"].asDouble() < 0.0 ||
            raw.body["top_p"].asDouble() > 1.0) {
            return invalid("top_p must be between 0 and 1");
        }
        request.topP = raw.body["top_p"].asDouble();
    }

    if (raw.body.isMember("system")) {
        std::string systemText;
        std::string parseError;
        if (!parseSystemPromptContent(raw.body["system"], systemText, parseError)) {
            return invalid(parseError);
        }
        appendSystemPrompt(request.systemPrompt, systemText);
    }

    request.protocolExtensions["claude"]["raw_request_fields"] = Json::Value(Json::objectValue);
    const char* extensionFields[] = {
        "stop_sequences", "top_k", "thinking", "metadata", "service_tier",
        "output_config", "context_management"
    };
    for (const char* field : extensionFields) {
        if (!raw.body.isMember(field)) continue;
        if (std::string(field) == "metadata") {
            appendSafeMetadata(raw.body[field], request.protocolExtensions["claude"][field]);
        } else {
            request.protocolExtensions["claude"]["raw_request_fields"][field] = raw.body[field];
        }
    }

    const auto& tools = raw.body["tools"];
    if (raw.body.isMember("tools") && !tools.isArray()) {
        return invalid("tools must be an array");
    }
    if (tools.isArray()) {
        request.protocolExtensions["claude"]["raw_tools"] = tools;
        for (const auto& tool : tools) {
            if (!tool.isObject() || tool.get("name", "").asString().empty()) {
                return invalid("each tool must have a name");
            }
            ToolDefinition definition;
            definition.name = tool["name"].asString();
            definition.description = tool.get("description", "").asString();
            definition.inputSchema = tool.get("input_schema", Json::Value(Json::objectValue));
            if (!definition.inputSchema.isObject()) {
                return invalid("tool input_schema must be an object");
            }
            definition.kind = tool.get("type", "function").asString() == "custom"
                ? ToolDefinitionKind::Custom : ToolDefinitionKind::Function;
            request.toolDefinitions.push_back(std::move(definition));
        }
    }

    if (raw.body.isMember("tool_choice")) {
        const auto& choice = raw.body["tool_choice"];
        if (!choice.isObject()) return invalid("tool_choice must be an object");
        const auto type = choice.get("type", "").asString();
        if (type == "auto") request.toolChoiceSpec.mode = ToolChoiceMode::Auto;
        else if (type == "none") request.toolChoiceSpec.mode = ToolChoiceMode::None;
        else if (type == "any") request.toolChoiceSpec.mode = ToolChoiceMode::Any;
        else if (type == "tool") {
            const auto name = choice.get("name", "").asString();
            if (name.empty()) return invalid("tool_choice type tool requires name");
            request.toolChoiceSpec.mode = ToolChoiceMode::Specific;
            request.toolChoiceSpec.toolName = name;
        } else {
            return invalid("unsupported Claude tool_choice type: " + type);
        }
        if (choice.isMember("disable_parallel_tool_use") &&
            !choice["disable_parallel_tool_use"].isBool()) {
            return invalid("disable_parallel_tool_use must be boolean");
        }
        if (choice.get("disable_parallel_tool_use", false).asBool()) {
            request.parallelToolCalls = false;
        }
    }

    request.continuityTexts.clear();
    std::vector<Message> parsedMessages;
    parsedMessages.reserve(raw.body["messages"].size());
    int lastAssistant = -1;
    for (Json::ArrayIndex i = 0; i < raw.body["messages"].size(); ++i) {
        const auto& rawMessage = raw.body["messages"][i];
        if (!rawMessage.isObject()) return invalid("messages entries must be objects");
        const auto role = rawMessage.get("role", "").asString();
        if (role != "user" && role != "assistant" && role != "system") {
            return invalid("message role must be user, assistant, or system (got " + role + ")");
        }
        Message message;
        if (role == "system") {
            std::string systemText;
            std::string parseError;
            if (!parseSystemPromptContent(rawMessage.get("content", Json::Value()),
                                           systemText, parseError)) {
                return invalid(parseError);
            }
            appendSystemPrompt(request.systemPrompt, systemText);
            message.role = MessageRole::System;
            appendTextBlock(message, systemText);
        } else {
            message.role = role == "assistant" ? MessageRole::Assistant : MessageRole::User;
            std::string parseError;
            if (!appendContentBlocks(rawMessage.get("content", Json::Value()), message,
                                     nullptr, parseError)) {
                return invalid(parseError);
            }
            request.continuityTexts.push_back(textFromContent(rawMessage.get("content", "")));
        }
        if (role == "assistant") lastAssistant = static_cast<int>(i);
        parsedMessages.push_back(std::move(message));
    }

    std::vector<ImageInfo> currentImages;
    for (int i = 0; i < static_cast<int>(parsedMessages.size()); ++i) {
        if (parsedMessages[i].role == MessageRole::System) continue;
        const auto& rawMessage = raw.body["messages"][static_cast<Json::ArrayIndex>(i)];
        const bool current = i > lastAssistant;
        if (!current) {
            Message history;
            history.role = parsedMessages[i].role;
            std::string parseError;
            if (!appendContentBlocks(rawMessage["content"], history, nullptr, parseError)) {
                return invalid(parseError);
            }
            appendHistoryMessages(history, request.messages);
            continue;
        }

        std::string parseError;
        Message currentMessage;
        currentMessage.role = parsedMessages[i].role;
        if (!appendContentBlocks(rawMessage["content"], currentMessage, &currentImages, parseError)) {
            return invalid(parseError);
        }
        appendCurrentInputMessage(request, currentMessage, raw.headers.userAgent);
    }
    request.images = std::move(currentImages);

    bool hasReplayableText = false;
    bool auxiliaryOnly = true;
    for (const auto& part : request.currentInputParts) {
        if (part.isToolResult || !part.isReplayableText) continue;
        hasReplayableText = true;
        auxiliaryOnly = auxiliaryOnly && part.isAuxiliary;
    }
    request.currentTurnKind = hasReplayableText && auxiliaryOnly
        ? CurrentTurnKind::Auxiliary : CurrentTurnKind::Durable;
    return AdapterResult{std::move(request), {}};
}

}  // namespace generation::protocol::claude
