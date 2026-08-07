#include "sessionManager/actionProtocol/ActionProtocolCompiler.h"

#include <json/json.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace actionproto {
namespace {

std::string trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool consume(std::string_view& input, std::string_view token) {
    if (input.size() < token.size() || input.substr(0, token.size()) != token) {
        return false;
    }
    input.remove_prefix(token.size());
    return true;
}

void consumeWhitespace(std::string_view& input) {
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front()))) {
        input.remove_prefix(1);
    }
}

CompileResult failure(CompileError code, const std::string& message, size_t offset = 0) {
    CompileResult result;
    result.matched = true;
    result.valid = false;
    result.diagnostic.code = code;
    result.diagnostic.message = message;
    result.diagnostic.offset = offset;
    return result;
}

bool parseObject(const std::string& json, Json::Value& value) {
    Json::CharReaderBuilder builder;
    builder["allowComments"] = false;
    builder["failIfExtra"] = true;
    builder["rejectDupKeys"] = true;
    std::string errors;
    std::istringstream stream(json);
    return Json::parseFromStream(builder, stream, &value, &errors) && value.isObject();
}

std::string compactJson(const Json::Value& value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

CompileResult compileJsonAction(
    const std::string& json,
    const CompileOptions& options) {
    Json::Value root;
    if (!parseObject(json, root)) {
        return failure(CompileError::InvalidEnvelope,
                       "action-v3 response must be one valid JSON object");
    }
    if (!root.isMember("protocol") || !root["protocol"].isString() ||
        root["protocol"].asString() != "action-v3") {
        return failure(CompileError::InvalidEnvelope,
                       "action-v3 JSON must declare protocol=action-v3");
    }

    const bool hasToolCalls = root.isMember("tool_calls");
    const bool hasFinalResponse = root.isMember("final_response");
    if (hasToolCalls == hasFinalResponse) {
        return failure(CompileError::MissingAction,
                       "action-v3 JSON must contain exactly one of tool_calls or final_response");
    }

    ActionEnvelope envelope;
    envelope.protocolVersion = 3;
    envelope.nonce = options.expectedSentinel;

    if (hasFinalResponse) {
        if (!root["final_response"].isString()) {
            return failure(CompileError::InvalidActionShape,
                           "action-v3 final_response must be a string");
        }
        envelope.finalResponse = root["final_response"].asString();
    } else {
        const auto& calls = root["tool_calls"];
        if (!calls.isArray() || calls.empty()) {
            return failure(CompileError::MissingAction,
                           "action-v3 tool_calls must be a non-empty array");
        }
        if (options.capabilities.maxToolCalls > 0 &&
            calls.size() > options.capabilities.maxToolCalls) {
            return failure(CompileError::MultipleActions,
                           "action-v3 response exceeds the allowed tool_calls for this turn");
        }
        for (const auto& call : calls) {
            if (!call.isObject()) {
                return failure(CompileError::InvalidActionShape,
                               "action-v3 tool_call must be an object");
            }
            if (!call.isMember("name") || !call["name"].isString() ||
                trim(call["name"].asString()).empty()) {
                return failure(CompileError::InvalidActionShape,
                               "action-v3 tool_call name must be a non-empty string");
            }
            if (!call.isMember("arguments") || !call["arguments"].isObject()) {
                return failure(CompileError::InvalidArgumentsJson,
                               "action-v3 tool_call arguments must be a JSON object");
            }

            ToolAction action;
            action.id = "action_" + std::to_string(envelope.toolCalls.size());
            action.name = trim(call["name"].asString());
            action.argumentsJson = compactJson(call["arguments"]);
            envelope.toolCalls.push_back(std::move(action));
            if (envelope.toolCalls.size() > options.capabilities.maxToolCalls) {
                return failure(CompileError::MultipleActions,
                               "tool_calls exceeds the client capability limit");
            }
        }
    }

    CompileResult result;
    result.matched = true;
    result.valid = true;
    result.envelope = std::move(envelope);
    return result;
}

} // namespace

// 家族的稳定可读名，仅用于日志与诊断输出。
// 不要用它反向解析或再做字符串比较——判定一律走 ClientCapabilities 字段。
const char* clientFamilyName(ClientFamily family) {
    switch (family) {
        case ClientFamily::RooKilo: return "RooKilo";
        case ClientFamily::Codex: return "Codex";
        case ClientFamily::ClaudeCode: return "ClaudeCode";
        case ClientFamily::Generic: break;
    }
    return "Generic";
}

// clientType 字符串的**唯一**解析点（详见头文件顶部的硬性规则）。
// 归一化策略：剔除分隔符（- _ 空格 .）后统一转小写，因此
//   "Kilo-Code" / "kilo_code" / "KILOCODE" / "kilo" 全部收敛到 RooKilo。
// 未识别的客户端一律落到 Generic —— 采取「不认识就按标准 OpenAI 行为处理」
// 的保守策略，避免误套用某个家族的特殊约束而破坏正常客户端。
ClientFamily normalizeClientType(const std::string& clientType) {
    // clientType 字符串的唯一解析点。大小写与连字符变体在此统一收敛，
    // 下游模块不得再自行比较字符串。
    std::string key;
    key.reserve(clientType.size());
    for (char c : clientType) {
        if (c == '-' || c == '_' || c == ' ' || c == '.') {
            continue;
        }
        key.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }

    if (key == "roocode" || key == "roo" || key == "kilocode" || key == "kilo") {
        return ClientFamily::RooKilo;
    }
    if (key == "codex") {
        return ClientFamily::Codex;
    }
    if (key == "claudecode" || key == "claude") {
        return ClientFamily::ClaudeCode;
    }
    return ClientFamily::Generic;
}

// 家族能力矩阵：本函数是所有客户端行为差异的**唯一声明处**。
// 新接入一个客户端时的标准流程：
//   1. 在 normalizeClientType 增加别名识别；
//   2. 在此 switch 增加一个 case，只写与默认值不同的字段；
//   3. 若出现现有字段无法表达的差异，回头在头文件新增能力位——
//      切勿在调用点重新引入 clientType 字符串比较。
// 默认值（Generic）即标准 OpenAI 兼容语义，故各 case 都是「差异清单」。
ClientCapabilities capabilitiesForFamily(ClientFamily family,
                                          bool parallelToolCalls) {
    ClientCapabilities capabilities;
    capabilities.family = family;
    capabilities.supportsParallelCalls = parallelToolCalls;
    capabilities.maxToolCalls = parallelToolCalls ? 16 : 1;

    switch (family) {
        case ClientFamily::RooKilo:
            // Roo/Kilo 只消费工具调用：final_response 会在适配层被还原成
            // attempt_completion，因此这里声明为不支持裸文本收尾。
            capabilities.requiresActionEveryTurn = true;
            capabilities.supportsFinalText = false;
            capabilities.supportsParallelCalls = false;
            capabilities.maxToolCalls = 1;
            capabilities.completionToolName = "attempt_completion";
            capabilities.prefersXmlWire = true;
            capabilities.allowsProseWithToolCall = false;
            capabilities.requiresStrictToolNames = true;
            capabilities.coalescesParallelCalls = true;
            break;
        case ClientFamily::Codex:
            // Codex 支持 freeform/custom 工具定义。
            capabilities.supportsCustomTools = true;
            // Codex 在未开启并行时要求把多调用收敛为首个调用。
            capabilities.coalescesParallelCalls = !parallelToolCalls;
            // Codex 客户端一旦读到 finish_reason="tool_calls" 即结束本轮文本消费，
            // 故零宽会话 ID 之类的带外文本必须早于工具调用事件下发，否则会被丢弃。
            capabilities.stopsConsumingTextAfterToolCall = true;
            break;
        case ClientFamily::ClaudeCode:
            // Claude Code 的约束与 Codex 同构：自定义工具 + 文本须前置。
            capabilities.supportsCustomTools = true;
            capabilities.coalescesParallelCalls = !parallelToolCalls;
            capabilities.stopsConsumingTextAfterToolCall = true;
            break;
        case ClientFamily::Generic:
            // 标准 OpenAI 兼容客户端：全部沿用结构体默认值，不做任何特化。
            break;
    }
    return capabilities;
}

ClientCapabilities capabilitiesForClient(const std::string& clientType,
                                          bool parallelToolCalls) {
    return capabilitiesForFamily(normalizeClientType(clientType),
                                 parallelToolCalls);
}

CompileResult ActionProtocolCompiler::compileResponse(
    const std::string& input,
    const CompileOptions& options) {
    const std::string source = trim(input);
    if (source.empty()) {
        return {};
    }

    if (!options.expectedSentinel.empty()) {
        if (source.rfind(options.expectedSentinel, 0) != 0) {
            return failure(CompileError::MissingSentinel,
                           "action protocol does not start with the expected sentinel");
        }
    }

    size_t start = options.expectedSentinel.size();
    std::string_view rest(source);
    rest.remove_prefix(std::min(start, rest.size()));
    consumeWhitespace(rest);

    // action-v3 uses one JSON object after the per-request sentinel. XML
    // action-v2 remains accepted below for existing sessions and providers.
    if (options.wireFormat == WireFormat::JsonV3 &&
        (rest.empty() || rest.front() != '{')) {
        return failure(CompileError::InvalidEnvelope,
                       "expected action-v3 JSON for the configured bridge format");
    }
    if (options.wireFormat == WireFormat::XmlV2 &&
        !rest.empty() && rest.front() == '{') {
        return failure(CompileError::InvalidEnvelope,
                       "expected action-v2 XML for the configured bridge format");
    }
    if (!rest.empty() && rest.front() == '{') {
        return compileJsonAction(std::string(rest), options);
    }

    constexpr std::string_view kOpen = "<action_protocol version=\"1\">";
    constexpr std::string_view kClose = "</action_protocol>";
    constexpr std::string_view kEnd = "<end_action/>";
    if (!consume(rest, kOpen)) {
        return failure(CompileError::InvalidEnvelope,
                       "missing <action_protocol version=\"1\"> envelope");
    }

    const std::string remaining(rest);
    const size_t closePos = remaining.rfind(kClose);
    if (closePos == std::string::npos) {
        return failure(CompileError::InvalidEnvelope,
                       "missing </action_protocol> closing tag");
    }
    const std::string body = remaining.substr(0, closePos);
    const std::string trailer = trim(remaining.substr(closePos + kClose.size()));
    if (trailer != kEnd) {
        return failure(CompileError::InvalidEnvelope,
                       "action protocol has unexpected trailing content", closePos);
    }

    ActionEnvelope envelope;
    envelope.nonce = options.expectedSentinel;
    std::string_view content(body);
    consumeWhitespace(content);

    constexpr std::string_view kFinalOpen = "<final_response><![CDATA[";
    constexpr std::string_view kFinalClose = "]]></final_response>";
    if (consume(content, kFinalOpen)) {
        const std::string finalBody(content);
        const size_t finalClose = finalBody.rfind(kFinalClose);
        if (finalClose == std::string::npos || !trim(finalBody.substr(finalClose + kFinalClose.size())).empty()) {
            return failure(CompileError::InvalidActionShape,
                           "final_response is missing its closing CDATA/tag");
        }
        envelope.finalResponse = finalBody.substr(0, finalClose);
        CompileResult result;
        result.matched = true;
        result.valid = true;
        result.envelope = std::move(envelope);
        return result;
    }

    constexpr std::string_view kCallsOpen = "<tool_calls>";
    constexpr std::string_view kCallsClose = "</tool_calls>";
    if (!consume(content, kCallsOpen)) {
        return failure(CompileError::MissingAction,
                       "action protocol must contain tool_calls or final_response");
    }

    while (true) {
        consumeWhitespace(content);
        if (consume(content, kCallsClose)) {
            break;
        }
        constexpr std::string_view kCallOpen = "<tool_call>";
        constexpr std::string_view kCallClose = "</tool_call>";
        constexpr std::string_view kNameOpen = "<name>";
        constexpr std::string_view kNameClose = "</name>";
        constexpr std::string_view kArgsOpen = "<arguments_json><![CDATA[";
        constexpr std::string_view kArgsClose = "]]></arguments_json>";

        if (!consume(content, kCallOpen)) {
            return failure(CompileError::InvalidActionShape,
                           "unexpected content inside <tool_calls>");
        }
        consumeWhitespace(content);
        if (!consume(content, kNameOpen)) {
            return failure(CompileError::InvalidActionShape,
                           "tool_call is missing <name>");
        }
        const size_t nameEnd = content.find(kNameClose);
        if (nameEnd == std::string_view::npos) {
            return failure(CompileError::InvalidActionShape,
                           "tool_call name is not closed");
        }
        const std::string name = trim(std::string(content.substr(0, nameEnd)));
        content.remove_prefix(nameEnd + kNameClose.size());
        if (name.empty()) {
            return failure(CompileError::InvalidActionShape,
                           "tool_call name is empty");
        }
        consumeWhitespace(content);
        if (!consume(content, kArgsOpen)) {
            return failure(CompileError::InvalidActionShape,
                           "tool_call is missing CDATA arguments_json");
        }
        const size_t argsEnd = content.find(kArgsClose);
        if (argsEnd == std::string_view::npos) {
            return failure(CompileError::InvalidArgumentsJson,
                           "arguments_json is not closed");
        }
        const std::string arguments(content.substr(0, argsEnd));
        content.remove_prefix(argsEnd + kArgsClose.size());
        Json::Value parsed;
        if (!parseObject(arguments, parsed)) {
            return failure(CompileError::InvalidArgumentsJson,
                           "arguments_json must be a valid JSON object");
        }
        consumeWhitespace(content);
        if (!consume(content, kCallClose)) {
            return failure(CompileError::InvalidActionShape,
                           "tool_call is not closed");
        }
        ToolAction action;
        action.id = "action_" + std::to_string(envelope.toolCalls.size());
        action.name = name;
        action.argumentsJson = arguments;
        envelope.toolCalls.push_back(std::move(action));
        if (envelope.toolCalls.size() > options.capabilities.maxToolCalls) {
            return failure(CompileError::MultipleActions,
                           "tool_calls exceeds the client capability limit");
        }
    }

    consumeWhitespace(content);
    if (!content.empty() || envelope.toolCalls.empty()) {
        return failure(envelope.toolCalls.empty() ? CompileError::MissingAction
                                                   : CompileError::InvalidActionShape,
                       envelope.toolCalls.empty() ? "tool_calls is empty"
                                                   : "unexpected content after tool_calls");
    }
    CompileResult result;
    result.matched = true;
    result.valid = true;
    result.envelope = std::move(envelope);
    return result;
}

std::string ActionProtocolCompiler::buildRouterPolicy(
    const std::string& sentinel,
    const ClientCapabilities& capabilities) {
    std::ostringstream policy;
    // 这组短句沿用 Roo/Kilo 已验证的提示词顺序和措辞。相比先描述抽象
    // “协议编译器”，上游模型更容易把它识别成可执行的工具路由任务。
    policy << "Context: Software engineering collaboration\n";
    policy << "Task: Generate the next tool call instruction (tools are executed by an external system)\n";
    policy << "Goal: Based on the user request, emit exactly 1 action.\n";
    policy << "Note: Do NOT explain whether you have access/permissions. Do NOT ask the user to paste file contents. Use the listed tools directly (e.g., call read_file/list_files when you need files).\n";
    policy << "Every response MUST contain exactly 1 action.\n";
    policy << "An action is either tool_calls (external state is required) or final_response (the task is finished).\n";
    policy << "Never emit both, and never emit neither.\n";
    if (!capabilities.supportsFinalText) {
        policy << "final_response is converted into the client completion tool call.\n";
    }
    policy << "\n";
    policy << "Transport contract:\n";
    policy << "Your entire response MUST be the exact sentinel followed by one JSON object.\n";
    policy << "Output no prose, markdown, analysis, prefix, or suffix outside the envelope.\n";
    policy << "Use a listed tool whenever external state is needed.\n";
    policy << "Never invent tool names or argument names.\n";
    policy << "The first line must be the exact sentinel: " << sentinel << "\n";
    policy << "If a tool is needed, use this exact JSON shape:\n";
    policy << sentinel
           << "\n{\"protocol\":\"action-v3\",\"tool_calls\":[{\"name\":\"TOOL_NAME\",\"arguments\":{\"PARAM\":\"VALUE\"}}]}\n";
    policy << "arguments MUST be a JSON object, never a JSON-encoded string.\n";
    policy << "If no tool is needed, put the complete answer in final_response using this exact JSON shape:\n";
    policy << sentinel
           << "\n{\"protocol\":\"action-v3\",\"final_response\":\"FINAL_ANSWER\"}\n";
    policy << "Escape quotes, backslashes, control characters, and newlines according to JSON.\n";
    policy << "Never put final prose in tool_calls or arguments.\n";
    if (capabilities.maxToolCalls == 1) {
        policy << "Exactly one tool_call is allowed.\n";
    } else {
        policy << "At most " << capabilities.maxToolCalls
               << " independent tool_calls are allowed.\n";
    }
    if (capabilities.requiresActionEveryTurn) {
        policy << "Plain text outside an action is discarded by the client.\n";
    }
    return policy.str();
}

} // namespace actionproto
