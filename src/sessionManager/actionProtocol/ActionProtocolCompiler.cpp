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
    std::string errors;
    std::istringstream stream(json);
    return Json::parseFromStream(builder, stream, &value, &errors) && value.isObject();
}

} // namespace

ClientCapabilities capabilitiesForClient(const std::string& clientType,
                                          bool parallelToolCalls) {
    ClientCapabilities capabilities;
    capabilities.supportsParallelCalls = parallelToolCalls;
    capabilities.maxToolCalls = parallelToolCalls ? 16 : 1;

    if (clientType == "RooCode" || clientType == "Kilo-Code") {
        capabilities.requiresActionEveryTurn = true;
        capabilities.maxToolCalls = 1;
    } else if (clientType == "Codex") {
        capabilities.supportsCustomTools = true;
    } else if (clientType == "ClaudeCode" || clientType == "Claude-Code") {
        capabilities.supportsCustomTools = true;
        capabilities.maxToolCalls = parallelToolCalls ? 16 : 1;
    }
    return capabilities;
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
    policy << "Goal: Based on the user request, select and output exactly 1 tool to call.\n";
    policy << "Note: Do NOT explain whether you have access/permissions. Do NOT ask the user to paste file contents. Use the listed tools directly (e.g., call read_file/list_files when you need files).\n";
    policy << "Each response MUST output exactly 1 action protocol action.\n";
    policy << "If no other tool is needed, use final_response (the Codex equivalent of attempt_completion) to output the final result.\n\n";
    policy << "Transport contract:\n";
    policy << "Your entire response MUST be exactly one action protocol envelope.\n";
    policy << "Output no prose, markdown, analysis, prefix, or suffix outside the envelope.\n";
    policy << "Use a listed tool whenever external state is needed.\n";
    policy << "Never invent tool names or argument names.\n";
    policy << "The first line must be the exact sentinel: " << sentinel << "\n";
    policy << "If a tool is needed, use this exact shape:\n";
    policy << sentinel << "\n<action_protocol version=\"1\">\n<tool_calls>\n"
           << "  <tool_call>\n    <name>TOOL_NAME</name>\n"
           << "    <arguments_json><![CDATA[{\"PARAM\":\"VALUE\"}]]></arguments_json>\n"
           << "  </tool_call>\n</tool_calls>\n</action_protocol>\n<end_action/>\n";
    policy << "If no tool is needed, use final_response; put the complete answer inside CDATA.\n";
    policy << "Never put final prose in arguments_json.\n";
    policy << "Do not put the sequence ]]> inside final_response; split it if necessary.\n";
    policy << sentinel << "\n<action_protocol version=\"1\">\n<final_response><![CDATA[FINAL_ANSWER]]></final_response>\n"
           << "</action_protocol>\n<end_action/>\n";
    if (capabilities.maxToolCalls == 1) {
        policy << "Exactly one tool_call is allowed.\n";
    } else {
        policy << "At most " << capabilities.maxToolCalls
               << " independent tool_calls are allowed.\n";
    }
    return policy.str();
}

} // namespace actionproto
