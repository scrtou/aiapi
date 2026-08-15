#include <application/generation/core/GenerationResponsePipeline.h>
#include <application/generation/core/ClientOutputSanitizer.h>
#include <application/generation/continuity/ContinuityResolver.h>
#include <application/generation/tooling/BridgeProtocolCodec.h>
#include <application/generation/tooling/ToolCallValidator.h>
#include <application/generation/tooling/StrictClientRules.h>
#include <application/generation/tooling/BridgeHelpers.h>
#include <application/generation/tooling/ForcedToolCallGenerator.h>
#include <application/generation/tooling/ToolCallNormalizer.h>
#include <application/generation/tooling/ToolDefinitionResolver.h>
#include <application/generation/actionProtocol/ActionProtocolCompiler.h>
#include <domain/port/IChannelCatalog.h>
#include <json/json.h>
#include <platform/Log.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <application/generation/core/Session.h>

using namespace bridge;

namespace {

struct ToolChoiceSpec {
    bool none = false;
    bool required = false;
    std::string forcedToolName;
};

struct ResponseAssembly {
    std::string clientType;
    actionproto::ClientCapabilities clientCapabilities;
    bool strictToolClient = false;
    bool codexRooCompat = false;
    bool strictSentinel = false;
    ToolChoiceSpec toolChoice;
    std::string textContent;
    std::vector<generation::ToolCallDone> toolCalls;
};

bool containsString(const Json::Value& values, const std::string& value)
{
    if (!values.isArray() || value.empty()) return false;
    for (const auto& item : values) {
        if (item.isString() && item.asString() == value) return true;
    }
    return false;
}

ToolChoiceSpec parseToolChoiceSpec(const std::string& encoded)
{
    ToolChoiceSpec spec;
    if (encoded.empty()) return spec;
    if (encoded.front() != '{') {
        const std::string mode = toLowerStr(encoded);
        spec.none = mode == "none";
        spec.required = mode == "required";
        return spec;
    }

    Json::Value value;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(encoded);
    if (!Json::parseFromStream(builder, input, &value, &errors) || !value.isObject() ||
        value.get("type", "").asString() != "function" ||
        !value.isMember("function") || !value["function"].isObject()) {
        return spec;
    }
    const auto& function = value["function"];
    spec.forcedToolName = toolcall::makeBridgeToolName(
        function.get("namespace", value.get("namespace", "")).asString(),
        function.get("name", "").asString());
    spec.required = !spec.forcedToolName.empty();
    return spec;
}

bool isStrictSentinelEnabled(const session_st& session,
                             bool strictToolClient,
                             bool toolChoiceRequired,
                             const Json::Value& runtimeConfig)
{
    const auto& custom = runtimeConfig;
    if (!custom.isObject() || !custom.isMember("tool_bridge") ||
        !custom["tool_bridge"].isObject()) {
        return strictToolClient || toolChoiceRequired;
    }

    const auto& config = custom["tool_bridge"];
    bool strict = !config.isMember("strict_sentinel") ||
        !config["strict_sentinel"].isBool() || config["strict_sentinel"].asBool();
    if (config.isMember("strict_sentinel_by_channel") &&
        config["strict_sentinel_by_channel"].isObject()) {
        const auto& byChannel = config["strict_sentinel_by_channel"];
        if (byChannel.isMember(session.request.api) && byChannel[session.request.api].isBool()) {
            strict = byChannel[session.request.api].asBool();
        }
    }
    if (config.isMember("strict_sentinel_by_model") &&
        config["strict_sentinel_by_model"].isObject()) {
        const auto& byModel = config["strict_sentinel_by_model"];
        if (!session.request.model.empty() && byModel.isMember(session.request.model) &&
            byModel[session.request.model].isBool()) {
            strict = byModel[session.request.model].asBool();
        }
    }
    if (config.isMember("strict_sentinel_disabled_channels") &&
        containsString(config["strict_sentinel_disabled_channels"], session.request.api)) {
        strict = false;
    }
    if (config.isMember("strict_sentinel_enabled_channels") &&
        containsString(config["strict_sentinel_enabled_channels"], session.request.api)) {
        strict = true;
    }
    if (config.isMember("strict_sentinel_disabled_models") &&
        containsString(config["strict_sentinel_disabled_models"], session.request.model)) {
        strict = false;
    }
    if (config.isMember("strict_sentinel_enabled_models") &&
        containsString(config["strict_sentinel_enabled_models"], session.request.model)) {
        strict = true;
    }
    return (strictToolClient || toolChoiceRequired) ? true : strict;
}

const Json::Value& requestToolDefinitions(const session_st& session)
{
    return (!session.request.toolsRaw.isNull() && session.request.toolsRaw.isArray() &&
            session.request.toolsRaw.size() > 0)
        ? session.request.toolsRaw
        : session.request.tools;
}

std::optional<toolcall::ToolDefinitionMatch> resolveToolDefinition(
    const session_st& session,
    const generation::ToolCallDone& call)
{
    auto findIn = [&](const Json::Value& tools) -> std::optional<toolcall::ToolDefinitionMatch> {
        if (!call.namespacePath.empty() && !call.originalName.empty()) {
            auto match = toolcall::findToolDefinition(tools, call.namespacePath, call.originalName);
            if (match.has_value()) return match;
        }
        return call.name.empty() ? std::optional<toolcall::ToolDefinitionMatch>{}
                                 : toolcall::findToolDefinition(tools, call.name);
    };
    auto match = findIn(session.request.toolsRaw);
    return match.has_value() ? match : findIn(session.request.tools);
}

void annotateToolCallIdentities(const session_st& session,
                                std::vector<generation::ToolCallDone>& calls)
{
    for (auto& call : calls) {
        const auto match = resolveToolDefinition(session, call);
        if (!match.has_value()) {
            if (call.type.empty()) call.type = "function";
            continue;
        }
        call.name = match->bridgeName;
        call.originalName = match->originalName;
        call.namespacePath = match->namespacePath;
        call.type = match->type;
    }
}

ResponseAssembly makeAssembly(const session_st& session,
                              bool supportsToolCalls,
                              const Json::Value& runtimeConfig)
{
    ResponseAssembly assembly;
    assembly.clientType = safeJsonAsString(session.provider.clientInfo.get("client_type", ""), "");
    assembly.clientCapabilities = actionproto::capabilitiesForClient(
        assembly.clientType, session.request.parallelToolCalls);
    assembly.strictToolClient = assembly.clientCapabilities.isStrictToolClient();
    assembly.codexRooCompat = assembly.clientCapabilities.family == actionproto::ClientFamily::Codex &&
        !supportsToolCalls;
    assembly.toolChoice = parseToolChoiceSpec(session.request.toolChoice);
    assembly.strictSentinel = isStrictSentinelEnabled(
        session, assembly.strictToolClient || assembly.codexRooCompat,
        assembly.toolChoice.required, runtimeConfig);
    return assembly;
}

void collectNativeToolCalls(const session_st& session,
                            std::vector<generation::ToolCallDone>& calls)
{
    const auto& rawCalls = session.response.message["tool_calls"];
    if (!rawCalls.isArray()) return;
    int index = 0;
    for (const auto& raw : rawCalls) {
        if (!raw.isObject()) continue;
        generation::ToolCallDone call;
        call.id = raw.get("id", "").asString();
        call.originalName = raw.get("name", "").asString();
        call.namespacePath = raw.get("namespace", "").asString();
        call.name = toolcall::makeBridgeToolName(call.namespacePath, call.originalName);
        call.arguments = raw.get("arguments", "{}").asString();
        call.index = index++;
        if (!call.name.empty()) calls.push_back(std::move(call));
    }
}

toolcall::BridgePolicyOptions decodeOptions(const session_st& session,
                                            const std::string& clientType)
{
    toolcall::BridgePolicyOptions options;
    options.clientType = clientType;
    options.channel = session.request.api;
    options.model = session.request.model;
    options.sentinel = session.provider.toolBridgeTrigger;
    options.parallelToolCalls = session.request.parallelToolCalls;
    return options;
}

void recordMissingBridgeAction(const session_st& session,
                               const ResponseAssembly& assembly,
                               const std::string& text)
{
    const Json::Value& definitions = requestToolDefinitions(session);
    if (!definitions.isArray() || definitions.empty()) return;

    Json::Value detail;
    detail["client_type"] = assembly.clientType;
    detail["tool_choice"] = session.request.toolChoice.empty()
        ? "auto(default)" : session.request.toolChoice;
    detail["strict_sentinel"] = assembly.strictSentinel;
    detail["sentinel_present"] = !session.provider.toolBridgeTrigger.empty() &&
        text.find(session.provider.toolBridgeTrigger) != std::string::npos;
    detail["function_calls_present"] = text.find("<function_calls>") != std::string::npos;
    detail["function_call_present"] = text.find("<function_call>") != std::string::npos;
    detail["configured_format"] = toolcall::bridgeWireFormatName(session.provider.toolBridgeFormat);
    detail["tool_definition_count"] = static_cast<Json::UInt64>(definitions.size());
    recordWarnStat(session, metrics::Domain::TOOL_BRIDGE,
                   metrics::EventType::TOOLBRIDGE_XML_NOT_FOUND,
                   "响应中未找到本次请求配置格式的工具桥动作", detail,
                   text.substr(0, std::min(text.size(), size_t(1024))));
}

void decodeBridgeOutput(const session_st& session,
                        const std::string& text,
                        ResponseAssembly& assembly)
{
    std::string input = extractXmlInputForToolCalls(session, text, !assembly.strictSentinel);
    if (input.empty()) {
        recordMissingBridgeAction(session, assembly, text);
        assembly.textContent = text;
        return;
    }
    input = normalizeBridgeXml(std::move(input));
    const auto options = decodeOptions(session, assembly.clientType);
    auto decoded = toolcall::createBridgeProtocolCodec(session.provider.toolBridgeFormat)
        ->decodeResponse(input, options);
    if ((!decoded.matched || !decoded.valid) && session.provider.toolBridgeAllowFormatFallback) {
        const auto fallback = session.provider.toolBridgeFormat == toolcall::BridgeWireFormat::Json
            ? toolcall::BridgeWireFormat::Xml
            : toolcall::BridgeWireFormat::Json;
        auto fallbackDecoded = toolcall::createBridgeProtocolCodec(fallback)->decodeResponse(input, options);
        if (fallbackDecoded.matched && fallbackDecoded.valid) {
            LOG_WARN << "[生成服务][ToolBridge] 响应使用兼容格式解析: configured="
                     << toolcall::bridgeWireFormatName(session.provider.toolBridgeFormat)
                     << ", detected=" << fallbackDecoded.protocol;
            decoded = std::move(fallbackDecoded);
        }
    }
    if (!decoded.valid) {
        Json::Value detail;
        detail["configured_format"] = toolcall::bridgeWireFormatName(session.provider.toolBridgeFormat);
        detail["detected_protocol"] = decoded.protocol;
        detail["matched"] = decoded.matched;
        detail["error_code"] = static_cast<int>(decoded.diagnostic.code);
        detail["offset"] = static_cast<Json::UInt64>(decoded.diagnostic.offset);
        recordWarnStat(session, metrics::Domain::TOOL_BRIDGE,
                       metrics::EventType::TOOLBRIDGE_PARSE_ERROR,
                       "固定格式动作协议解析失败: " + decoded.diagnostic.message,
                       detail, text.substr(0, std::min(text.size(), size_t(2048))));
        assembly.textContent = text;
    } else {
        LOG_INFO << "[生成服务][ToolBridge] 响应解析成功: configured="
                 << toolcall::bridgeWireFormatName(session.provider.toolBridgeFormat)
                 << ", detected=" << decoded.protocol;
        assembly.toolCalls = std::move(decoded.toolCalls);
        assembly.textContent = std::move(decoded.text);
    }
    if (assembly.strictSentinel &&
        input.find(session.provider.toolBridgeTrigger) == std::string::npos) {
        assembly.toolCalls.clear();
        assembly.textContent = text;
    }
}

void extractResponseOutput(const session_st& session,
                           bool supportsToolCalls,
                           ResponseAssembly& assembly)
{
    const std::string text = ClientOutputSanitizer::sanitize(
        session.provider.clientInfo,
        safeJsonAsString(session.response.message.get("message", ""), ""));
    collectNativeToolCalls(session, assembly.toolCalls);
    if (!assembly.toolCalls.empty() || supportsToolCalls || assembly.toolChoice.none) {
        assembly.textContent = text;
        return;
    }
    decodeBridgeOutput(session, text, assembly);
    if (assembly.toolCalls.empty()) {
        toolcall::generateForcedToolCall(session, assembly.toolCalls, assembly.textContent);
    }
}

void filterForcedTool(const session_st& session, ResponseAssembly& assembly)
{
    if (assembly.toolChoice.forcedToolName.empty() || assembly.toolCalls.empty()) return;
    std::vector<generation::ToolCallDone> accepted;
    std::vector<std::string> rejected;
    accepted.reserve(assembly.toolCalls.size());
    for (const auto& call : assembly.toolCalls) {
        if (call.name == assembly.toolChoice.forcedToolName) {
            accepted.push_back(call);
        } else {
            rejected.push_back(call.name);
        }
    }
    if (!rejected.empty()) {
        std::ostringstream names;
        for (size_t index = 0; index < rejected.size(); ++index) {
            if (index) names << ',';
            names << rejected[index];
        }
        Json::Value detail;
        detail["forced_tool_name"] = assembly.toolChoice.forcedToolName;
        detail["rejected_count"] = static_cast<Json::UInt64>(rejected.size());
        detail["rejected_tools"] = names.str();
        recordWarnStat(session, metrics::Domain::TOOL_BRIDGE,
                       metrics::EventType::TOOLBRIDGE_VALIDATION_FILTERED,
                       "tool_choice 指定函数约束生效，已过滤非指定工具调用", detail);
    }
    assembly.toolCalls = std::move(accepted);
}

void resolveCodexCompletion(ResponseAssembly& assembly)
{
    if (!assembly.codexRooCompat || assembly.toolCalls.empty()) return;
    std::vector<generation::ToolCallDone> executable;
    std::string completion;
    executable.reserve(assembly.toolCalls.size());
    for (const auto& call : assembly.toolCalls) {
        if (call.name != "attempt_completion") {
            executable.push_back(call);
            continue;
        }
        Json::Value arguments;
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream input(call.arguments);
        if (Json::parseFromStream(builder, input, &arguments, &errors) && arguments.isObject() &&
            arguments.isMember("result") && arguments["result"].isString() &&
            !arguments["result"].asString().empty() && completion.empty()) {
            completion = arguments["result"].asString();
        } else {
            LOG_WARN << "[生成服务][CodexRooCompat] attempt_completion 参数无效";
        }
    }
    if (!executable.empty()) {
        assembly.toolCalls = std::move(executable);
        assembly.textContent.clear();
    } else if (!completion.empty()) {
        assembly.toolCalls.clear();
        assembly.textContent = std::move(completion);
        LOG_INFO << "[生成服务][CodexRooCompat] 已将虚拟 attempt_completion 转换为最终文本";
    } else {
        assembly.toolCalls.clear();
    }
}

void validateToolCalls(const session_st& session, ResponseAssembly& assembly)
{
    const Json::Value& definitions = requestToolDefinitions(session);
    if (assembly.toolCalls.empty() || !definitions.isArray() || definitions.empty()) return;

    toolcall::ToolCallValidator validator(definitions, assembly.clientType);
    std::string discarded;
    const auto mode = toolcall::getRecommendedValidationMode(assembly.clientType);
    const size_t removed = validator.filterInvalidToolCalls(assembly.toolCalls, discarded, mode);
    if (removed == 0) return;

    LOG_WARN << "[生成服务] 通过 校验过滤了" << removed << " 个无效的工具调用";
    Json::Value detail;
    detail["removed_count"] = static_cast<Json::UInt64>(removed);
    detail["validation_mode"] = static_cast<int>(mode);
    recordWarnStat(session, metrics::Domain::TOOL_BRIDGE,
                   metrics::EventType::TOOLBRIDGE_VALIDATION_FILTERED,
                   "已过滤无效工具调用数量: " + std::to_string(removed), detail,
                   discarded.substr(0, std::min(discarded.size(), size_t(2048))));
    if (!assembly.toolCalls.empty()) return;

    toolcall::applyValidationFallback(assembly.clientType, assembly.toolCalls,
                                      assembly.textContent, discarded);
    recordWarnStat(session, metrics::Domain::TOOL_BRIDGE,
                   metrics::EventType::TOOLBRIDGE_VALIDATION_FALLBACK_APPLIED,
                   "已应用校验降级策略，客户端: " + assembly.clientType);
}

void applyClientRules(const session_st& session, ResponseAssembly& assembly)
{
    if (assembly.strictToolClient) {
        const size_t before = assembly.toolCalls.size();
        const bool hadText = !assembly.textContent.empty();
        toolcall::applyStrictClientRules(assembly.clientType, assembly.textContent,
                                         assembly.toolCalls);
        if (before != assembly.toolCalls.size() ||
            (hadText && assembly.textContent.empty() && !assembly.toolCalls.empty())) {
            Json::Value detail;
            detail["original_tool_calls"] = static_cast<Json::UInt64>(before);
            detail["final_tool_calls"] = static_cast<Json::UInt64>(assembly.toolCalls.size());
            detail["text_cleared"] = hadText && assembly.textContent.empty();
            recordWarnStat(session, metrics::Domain::TOOL_BRIDGE,
                           metrics::EventType::TOOLBRIDGE_STRICT_CLIENT_RULE_APPLIED,
                           "已应用严格客户端规则，客户端: " + assembly.clientType, detail);
        }
    }
    if (assembly.clientCapabilities.coalescesParallelCalls &&
        assembly.toolCalls.size() > assembly.clientCapabilities.maxToolCalls) {
        LOG_WARN << "[生成服务][ToolBridge] 客户端能力限制 maxToolCalls="
                 << assembly.clientCapabilities.maxToolCalls << "，已截断多余工具调用";
        assembly.toolCalls.erase(
            assembly.toolCalls.begin() + assembly.clientCapabilities.maxToolCalls,
            assembly.toolCalls.end());
    }
    for (size_t index = 0; index < assembly.toolCalls.size(); ++index) {
        assembly.toolCalls[index].index = static_cast<int>(index);
    }
}

void embedSessionIdIfNeeded(chatSession* sessionStore,
                            const session_st& session,
                            const actionproto::ClientCapabilities& capabilities,
                            ResponseAssembly& assembly,
                            IResponseSink& sink)
{
    if (!sessionStore || !sessionStore->isZeroWidthMode() ||
        continuity::hasStableClientSession(session.provider.clientInfo)) {
        return;
    }
    const std::string& id = session.state.nextSessionId.empty()
        ? session.state.conversationId
        : session.state.nextSessionId;
    if (id.empty()) return;

    if (!assembly.toolCalls.empty() && capabilities.stopsConsumingTextAfterToolCall) {
        const std::string marker = chatSession::embedSessionIdInText("", id);
        if (marker.empty()) return;
        generation::OutputTextDone event;
        event.text = marker;
        event.index = 0;
        sink.onEvent(event);
        LOG_INFO << "[生成服务] 已在 tool_calls 事件前发送零宽会话ID：" << id
                 << "（当前会话: " << session.state.conversationId << ")";
        return;
    }
    assembly.textContent = chatSession::embedSessionIdInText(assembly.textContent, id);
    LOG_INFO << "[生成服务] 已在响应中嵌入会话ID: " << id
             << "（当前会话: " << session.state.conversationId << ")";
}

void emitProtocolEvents(const session_st& session,
                        const ResponseAssembly& assembly,
                        IResponseSink& sink)
{
    for (const auto& call : assembly.toolCalls) sink.onEvent(call);
    if (!assembly.textContent.empty()) {
        generation::OutputTextDone text;
        text.text = assembly.textContent;
        text.index = 0;
        sink.onEvent(text);
    }
    generation::Completed completed;
    completed.finishReason = assembly.toolCalls.empty() ? "stop" : "tool_calls";
    if (session.response.message.isMember("_meta") && session.response.message["_meta"].isObject()) {
        completed.meta = session.response.message["_meta"];
    }
    sink.onEvent(completed);
}

} // namespace

generation::GenerationResponsePipeline::GenerationResponsePipeline(
    chatSession* sessionStore,
    IChannelCatalog* channelCatalog,
    Json::Value runtimeConfig) noexcept
    : sessionStore_(sessionStore),
      channelCatalog_(channelCatalog),
      runtimeConfig_(std::move(runtimeConfig))
{
}

void generation::GenerationResponsePipeline::emit(const session_st& session,
                                                   IResponseSink& sink) const
{
    const bool supportsToolCalls = channelSupportsToolCalls(session.request.api);
    ResponseAssembly assembly = makeAssembly(session, supportsToolCalls, runtimeConfig_);
    extractResponseOutput(session, supportsToolCalls, assembly);

    annotateToolCallIdentities(session, assembly.toolCalls);
    filterForcedTool(session, assembly);
    resolveCodexCompletion(assembly);
    if (assembly.toolCalls.empty() && assembly.toolChoice.required) {
        toolcall::generateForcedToolCall(session, assembly.toolCalls, assembly.textContent);
    }
    annotateToolCallIdentities(session, assembly.toolCalls);
    toolcall::normalizeToolCallArguments(session, assembly.toolCalls);
    validateToolCalls(session, assembly);
    applyClientRules(session, assembly);
    embedSessionIdIfNeeded(sessionStore_, session, assembly.clientCapabilities, assembly, sink);
    emitProtocolEvents(session, assembly, sink);
}

void generation::GenerationResponsePipeline::emitError(
    platform::ErrorCode code,
    const std::string& message,
    IResponseSink& sink) const
{
    emitError(platform::Error(code, message), sink);
}

void generation::GenerationResponsePipeline::emitError(
    const platform::Error& source,
    IResponseSink& sink) const
{
    generation::Error error;
    error.code = source.code;
    error.message = source.message;
    error.providerCode = source.providerCode;
    error.detail = source.detail;
    sink.onEvent(error);
}

bool generation::GenerationResponsePipeline::channelSupportsToolCalls(
    const std::string& channelName) const
{
    const auto result = channelCatalog_
        ? channelCatalog_->supportsToolCalls(channelName)
        : std::optional<bool>{};
    if (result.has_value()) {
        LOG_INFO << "[生成服务] 通道 " << channelName
                 << " supportsToolCalls: " << result.value();
        return result.value();
    }
    LOG_WARN << "[生成服务] 未找到通道 " << channelName << "，默认支持 工具调用";
    return true;
}
