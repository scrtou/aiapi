#include <application/generation/tooling/BridgeProtocolCodec.h>

#include <application/generation/actionProtocol/ActionProtocolAdapter.h>
#include <application/generation/tooling/BridgeHelpers.h>
#include <application/generation/tooling/ToolCallBridge.h>
#include <application/generation/tooling/XmlTagToolCallCodec.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace toolcall {
namespace {

std::string lower(std::string value) {
    for (auto& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string compactJson(const Json::Value& value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

std::string escapeXml(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string truncateDescription(const Json::Value& value, int maxChars) {
    if (!value.isString() || maxChars <= 0) return "";
    std::string text = trim(value.asString());
    if (static_cast<int>(text.size()) <= maxChars) return text;
    return text.substr(0, static_cast<size_t>(maxChars)) + "...";
}

Json::Value compactSchema(const Json::Value& schema) {
    if (!schema.isObject()) return Json::Value(Json::objectValue);
    Json::Value out(Json::objectValue);
    if (schema.isMember("type")) out["type"] = schema["type"];
    if (schema.isMember("required")) out["required"] = schema["required"];
    if (schema.isMember("additionalProperties")) {
        out["additionalProperties"] = schema["additionalProperties"];
    }
    if (schema.isMember("properties") && schema["properties"].isObject()) {
        Json::Value properties(Json::objectValue);
        for (const auto& key : schema["properties"].getMemberNames()) {
            const auto& source = schema["properties"][key];
            Json::Value property(Json::objectValue);
            if (source.isMember("type")) property["type"] = source["type"];
            if (source.isMember("enum")) property["enum"] = source["enum"];
            if (source.isMember("items")) property["items"] = compactSchema(source["items"]);
            properties[key] = std::move(property);
        }
        out["properties"] = std::move(properties);
    }
    return out;
}

Json::Value normalizedDefinitions(const Json::Value& tools,
                                  const BridgeDefinitionOptions& options) {
    Json::Value definitions(Json::arrayValue);
    if (!tools.isArray()) return definitions;
    // 不再硬夹上限：完全尊重配置 max_description_chars，仅做非负保护。
    const int maxChars = std::max(0, options.maxDescriptionChars);
    for (const auto& tool : tools) {
        if (!tool.isObject() || tool.get("type", "").asString() != "function" ||
            !tool.isMember("function") || !tool["function"].isObject()) {
            continue;
        }
        const auto& function = tool["function"];
        const std::string name = function.get("name", "").asString();
        if (name.empty()) continue;
        Json::Value definition(Json::objectValue);
        definition["name"] = name;
        if (options.includeDescriptions) {
            const std::string description = truncateDescription(
                function.get("description", Json::Value("")), maxChars);
            if (!description.empty()) definition["description"] = description;
        }
        const Json::Value parameters = function.get(
            "parameters", Json::Value(Json::objectValue));
        definition["parameters"] = options.fullSchema
            ? parameters : compactSchema(parameters);
        definitions.append(std::move(definition));
    }
    return definitions;
}

std::string toolNameFromCall(const Json::Value& call) {
    if (!call.isObject()) return "";
    if (call.isMember("function") && call["function"].isObject()) {
        return call["function"].get("name", "").asString();
    }
    return call.get("name", "").asString();
}

Json::Value toolArgumentsFromCall(const Json::Value& call) {
    Json::Value raw;
    if (call.isMember("function") && call["function"].isObject()) {
        raw = call["function"].get("arguments", Json::Value(Json::objectValue));
    } else {
        raw = call.get("arguments", Json::Value(Json::objectValue));
    }
    if (raw.isObject()) return raw;
    if (raw.isString()) {
        Json::Value parsed;
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream stream(raw.asString());
        if (Json::parseFromStream(builder, stream, &parsed, &errors) && parsed.isObject()) {
            return parsed;
        }
    }
    return Json::Value(Json::objectValue);
}

std::string toolIdFromCall(const Json::Value& call) {
    return call.get("id", call.get("call_id", "")).asString();
}

void transformHistoryCommon(const IBridgeProtocolCodec& codec,
                            Json::Value& messageContext,
                            const BridgePolicyOptions& options) {
    if (!messageContext.isArray()) return;
    for (auto& message : messageContext) {
        if (!message.isObject()) continue;
        const std::string role = message.get("role", "").asString();
        if (role == "assistant" && message.isMember("tool_calls") &&
            message["tool_calls"].isArray() && !message["tool_calls"].empty()) {
            std::string content = message.get("content", "").asString();
            const std::string encoded = codec.encodeToolCallsForHistory(
                message["tool_calls"], options);
            if (!content.empty() && !encoded.empty()) content += "\n";
            content += encoded;
            message["content"] = content;
            message.removeMember("tool_calls");
        } else if (role == "tool") {
            const std::string content = message.get("content", "").asString();
            message["role"] = "user";
            message["content"] = codec.encodeToolResultForHistory(
                message.get("tool_call_id", "").asString(), content);
            message.removeMember("tool_call_id");
        }
    }
}

BridgeDecodeResult adaptCompiled(const actionproto::CompileResult& compiled,
                                 const std::string& clientType,
                                 const std::string& protocol) {
    BridgeDecodeResult result;
    result.matched = compiled.matched;
    result.valid = compiled.valid;
    result.protocol = protocol;
    result.diagnostic = compiled.diagnostic;
    if (compiled.valid) {
        auto adapted = actionproto::adaptForCapabilities(
            compiled.envelope,
            actionproto::capabilitiesForClient(clientType,
                                               /*parallelToolCalls=*/false));
        result.toolCalls = std::move(adapted.toolCalls);
        result.text = std::move(adapted.text);
    }
    return result;
}

class JsonBridgeProtocolCodec final : public IBridgeProtocolCodec {
public:
    BridgeWireFormat format() const override { return BridgeWireFormat::Json; }

    std::string encodeToolDefinitions(
        const Json::Value& tools,
        const BridgeDefinitionOptions& options) const override {
        Json::Value root(Json::objectValue);
        root["tools"] = normalizedDefinitions(tools, options);
        return compactJson(root);
    }

    std::string buildPolicy(const BridgePolicyOptions& options) const override {
        auto capabilities = actionproto::capabilitiesForClient(
            options.clientType, options.parallelToolCalls);
        std::string policy = actionproto::ActionProtocolCompiler::buildRouterPolicy(
            options.sentinel, capabilities);
        if (!options.forcedToolName.empty()) {
            policy += "Required tool for this response: " + options.forcedToolName + ".\n";
        } else if (options.toolChoice == "required" ||
                   options.requireToolForCurrentRequest) {
            policy += "This request requires external inspection; use a real tool_call, not final_response.\n";
        } else {
            policy += "Use final_response only after all required tool work is complete.\n";
        }
        return policy;
    }

    std::string encodeToolCallsForHistory(
        const Json::Value& toolCalls,
        const BridgePolicyOptions& options) const override {
        Json::Value root(Json::objectValue);
        root["protocol"] = "action-v3";
        Json::Value calls(Json::arrayValue);
        if (toolCalls.isArray()) {
            for (const auto& source : toolCalls) {
                const std::string name = toolNameFromCall(source);
                if (name.empty()) continue;
                Json::Value call(Json::objectValue);
                call["name"] = name;
                call["arguments"] = toolArgumentsFromCall(source);
                const std::string id = toolIdFromCall(source);
                if (!id.empty()) call["id"] = id;
                calls.append(std::move(call));
            }
        }
        root["tool_calls"] = std::move(calls);
        return options.sentinel + "\n" + compactJson(root);
    }

    std::string encodeToolResultForHistory(
        const std::string& toolCallId,
        const std::string& content) const override {
        Json::Value root(Json::objectValue);
        root["type"] = "tool_result";
        if (!toolCallId.empty()) root["tool_call_id"] = toolCallId;
        root["content"] = content;
        return compactJson(root);
    }

    void transformHistory(Json::Value& messageContext,
                          const BridgePolicyOptions& options) const override {
        transformHistoryCommon(*this, messageContext, options);
    }

    BridgeDecodeResult decodeResponse(
        const std::string& input,
        const BridgePolicyOptions& options) const override {
        actionproto::CompileOptions compileOptions;
        compileOptions.expectedSentinel = options.sentinel;
        compileOptions.capabilities = actionproto::capabilitiesForClient(
            options.clientType, options.parallelToolCalls);
        compileOptions.wireFormat = actionproto::WireFormat::JsonV3;
        return adaptCompiled(
            actionproto::ActionProtocolCompiler::compileResponse(input, compileOptions),
            options.clientType, "action-v3-json");
    }

    std::string buildRetryPrompt(const BridgePolicyOptions& options) const override {
        return "Reply again with the exact trigger marker followed by exactly one action-v3 JSON object using the API Definitions already provided.\n"
               "Use a real tool_call when external state is required; otherwise use final_response.\n"
               "Output no XML, prose, explanation, markdown, or refusal.\n"
               "Exact trigger: " + options.sentinel + "\n";
    }
};

class XmlBridgeProtocolCodec final : public IBridgeProtocolCodec {
public:
    BridgeWireFormat format() const override { return BridgeWireFormat::Xml; }

    std::string encodeToolDefinitions(
        const Json::Value& tools,
        const BridgeDefinitionOptions& options) const override {
        const Json::Value definitions = normalizedDefinitions(tools, options);
        std::ostringstream out;
        out << "<functions>\n";
        for (const auto& definition : definitions) {
            out << "  <function>\n";
            out << "    <name>" << escapeXml(definition["name"].asString()) << "</name>\n";
            if (definition.isMember("description")) {
                out << "    <description>"
                    << escapeXml(definition["description"].asString())
                    << "</description>\n";
            }
            out << "    <parameters_json>"
                << escapeXml(compactJson(definition["parameters"]))
                << "</parameters_json>\n";
            out << "  </function>\n";
        }
        out << "</functions>";
        return out.str();
    }

    std::string buildPolicy(const BridgePolicyOptions& options) const override {
        const auto capabilities = actionproto::capabilitiesForClient(
            options.clientType, options.parallelToolCalls);
        // 严格性来自能力 IR；retoolapi 是通道级（非客户端级）强约束，保留。
        const bool strict = capabilities.isStrictToolClient() ||
                            options.channel == "retoolapi";
        std::ostringstream policy;
        policy << "Context: Software engineering collaboration\n";
        policy << "Task: Generate the next tool call instruction (tools are executed by an external system)\n";
        policy << "Use only the XML bridge format defined here.\n";
        policy << "Every response MUST contain exactly 1 action.\n";
        if (!options.forcedToolName.empty()) {
            policy << "Required tool for this response: " << options.forcedToolName << "\n";
        } else if (options.toolChoice == "required" || strict ||
                   options.requireToolForCurrentRequest) {
            policy << "The action MUST be a tool call.\n";
            const std::string completionTool =
                capabilities.requiresCompletionTool()
                    ? capabilities.completionToolName
                    : std::string("attempt_completion");
            policy << "When the task is finished, the action is "
                   << completionTool << ".\n";
            policy << "Never emit both a tool call and prose, and never emit neither.\n";
        } else {
            policy << "The action is a tool call when external state is required; otherwise it is your normal answer.\n";
            policy << "Never emit a tool call together with prose.\n";
        }
        policy << "When calling a tool, output ONLY this XML with no prefix or suffix:\n";
        policy << options.sentinel << "\n";
        policy << "<function_calls>\n"
                  "  <function_call>\n"
                  "    <tool>TOOL_NAME</tool>\n"
                  "    <args_json><![CDATA[{\"PARAM_NAME\":\"VALUE\"}]]></args_json>\n"
                  "  </function_call>\n"
                  "</function_calls>\n";
        policy << "Tool names and argument names must exactly match the definitions.\n";
        return policy.str();
    }

    std::string encodeToolCallsForHistory(
        const Json::Value& toolCalls,
        const BridgePolicyOptions& options) const override {
        std::ostringstream out;
        out << options.sentinel << "\n<function_calls>\n";
        if (toolCalls.isArray()) {
            for (const auto& source : toolCalls) {
                const std::string name = toolNameFromCall(source);
                if (name.empty()) continue;
                out << "  <function_call>\n"
                    << "    <tool>" << escapeXml(name) << "</tool>\n"
                    << "    <args_json><![CDATA["
                    << compactJson(toolArgumentsFromCall(source))
                    << "]]></args_json>\n"
                    << "  </function_call>\n";
            }
        }
        out << "</function_calls>";
        return out.str();
    }

    std::string encodeToolResultForHistory(
        const std::string& toolCallId,
        const std::string& content) const override {
        std::ostringstream out;
        out << "<tool_result";
        if (!toolCallId.empty()) {
            out << " tool_call_id=\"" << escapeXml(toolCallId) << "\"";
        }
        out << ">" << escapeXml(content) << "</tool_result>";
        return out.str();
    }

    void transformHistory(Json::Value& messageContext,
                          const BridgePolicyOptions& options) const override {
        transformHistoryCommon(*this, messageContext, options);
    }

    BridgeDecodeResult decodeResponse(
        const std::string& input,
        const BridgePolicyOptions& options) const override {
        const size_t sentinelPos = input.find(options.sentinel);
        if (sentinelPos == std::string::npos) return {};
        const std::string candidate = input.substr(sentinelPos);
        const size_t actionPos = candidate.find("<action_protocol");
        if (actionPos != std::string::npos) {
            actionproto::CompileOptions compileOptions;
            compileOptions.expectedSentinel = options.sentinel;
            compileOptions.capabilities = actionproto::capabilitiesForClient(
                options.clientType, options.parallelToolCalls);
            compileOptions.wireFormat = actionproto::WireFormat::XmlV2;
            return adaptCompiled(
                actionproto::ActionProtocolCompiler::compileResponse(
                    candidate, compileOptions),
                options.clientType, "action-v2-xml");
        }

        BridgeDecodeResult result;
        result.matched = candidate.find("<function_calls") != std::string::npos;
        result.protocol = "function-calls-xml";
        if (!result.matched) return result;

        auto bridge = createToolCallBridge(false);
        auto parser = createXmlTagToolCallCodec();
        parser->setSentinel(options.sentinel);
        bridge->setTextCodec(parser);
        std::vector<ToolCallEvent> events;
        bridge->transformResponseChunk(candidate, events);
        bridge->flushResponse(events);
        bool parseError = false;
        for (const auto& event : events) {
            if (event.type == EventType::Text) {
                result.text += event.text;
            } else if (event.type == EventType::ToolCallEnd) {
                generation::ToolCallDone call;
                call.id = event.toolCallId;
                call.name = event.toolName;
                call.arguments = event.argumentsDelta;
                call.index = static_cast<int>(result.toolCalls.size());
                result.toolCalls.push_back(std::move(call));
            } else if (event.type == EventType::Error) {
                parseError = true;
                result.diagnostic.code = actionproto::CompileError::InvalidEnvelope;
                result.diagnostic.message = event.errorMessage;
            }
        }
        result.valid = !parseError && !result.toolCalls.empty();
        if (!result.valid && result.diagnostic.message.empty()) {
            result.diagnostic.code = actionproto::CompileError::MissingAction;
            result.diagnostic.message = "XML response does not contain a complete tool call";
        }
        return result;
    }

    std::string buildRetryPrompt(const BridgePolicyOptions& options) const override {
        return "Reply again with the exact trigger marker followed by exactly one XML function_calls block using the API Definitions already provided.\n"
               "Output no JSON, prose, explanation, markdown, or refusal.\n"
               "Exact trigger: " + options.sentinel + "\n";
    }
};

BridgeWireFormat lookupFormat(const Json::Value& object,
                              const std::string& key,
                              BridgeWireFormat current) {
    if (!object.isObject() || key.empty() || !object.isMember(key) ||
        !object[key].isString()) {
        return current;
    }
    return parseBridgeWireFormat(object[key].asString(), current);
}

}  // namespace

const char* bridgeWireFormatName(BridgeWireFormat format) {
    switch (format) {
        case BridgeWireFormat::Json: return "json";
        case BridgeWireFormat::Xml: return "xml";
        case BridgeWireFormat::Unset: return "unset";
    }
    return "unset";
}

BridgeWireFormat parseBridgeWireFormat(const std::string& value,
                                       BridgeWireFormat fallback) {
    const std::string normalized = lower(trim(value));
    if (normalized == "json" || normalized == "action-v3" ||
        normalized == "json_action_v3") {
        return BridgeWireFormat::Json;
    }
    if (normalized == "xml" || normalized == "action-v2" ||
        normalized == "xml_action_v2") {
        return BridgeWireFormat::Xml;
    }
    return fallback;
}

BridgeWireFormat resolveBridgeWireFormat(const Json::Value& toolBridgeConfig,
                                         const std::string& clientType,
                                         const std::string& channel,
                                         const std::string& model) {
    // 默认线格式由能力 IR 决定：偏好 XML 的客户端走 action-v2，其余走 action-v3。
    const auto capabilities =
        actionproto::capabilitiesForClient(clientType, /*parallelToolCalls=*/false);
    BridgeWireFormat format = capabilities.prefersXmlWire
        ? BridgeWireFormat::Xml
        : (capabilities.family == actionproto::ClientFamily::Codex
               ? BridgeWireFormat::Json
               : BridgeWireFormat::Xml);
    if (!toolBridgeConfig.isObject()) return format;
    if (toolBridgeConfig.isMember("format") &&
        toolBridgeConfig["format"].isString()) {
        format = parseBridgeWireFormat(toolBridgeConfig["format"].asString(), format);
    }
    format = lookupFormat(toolBridgeConfig["format_by_channel"], channel, format);
    format = lookupFormat(toolBridgeConfig["format_by_client"], clientType, format);
    format = lookupFormat(toolBridgeConfig["format_by_model"], model, format);
    return format;
}

bool resolveBridgeFormatFallback(const Json::Value& toolBridgeConfig) {
    return toolBridgeConfig.isObject() &&
           toolBridgeConfig.get("allow_format_fallback", false).asBool();
}

std::shared_ptr<IBridgeProtocolCodec> createBridgeProtocolCodec(
    BridgeWireFormat format) {
    if (format == BridgeWireFormat::Xml) {
        return std::make_shared<XmlBridgeProtocolCodec>();
    }
    return std::make_shared<JsonBridgeProtocolCodec>();
}

}  // namespace toolcall
