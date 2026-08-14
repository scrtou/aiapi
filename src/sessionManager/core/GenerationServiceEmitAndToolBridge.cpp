#include <sessionManager/core/GenerationService.h>
#include <sessionManager/core/ClientOutputSanitizer.h>
#include <sessionManager/continuity/ContinuityResolver.h>
#include <sessionManager/continuity/ResponseIndex.h>
#include <sessionManager/tooling/ToolCallBridge.h>
#include <sessionManager/tooling/BridgeProtocolCodec.h>
#include <sessionManager/tooling/ToolCallValidator.h>
#include <sessionManager/tooling/XmlTagToolCallCodec.h>
#include <sessionManager/tooling/StrictClientRules.h>
#include <sessionManager/tooling/BridgeHelpers.h>
#include <sessionManager/tooling/ForcedToolCallGenerator.h>
#include <sessionManager/tooling/ToolCallNormalizer.h>
#include <sessionManager/tooling/ToolDefinitionEncoder.h>
#include <sessionManager/tooling/ToolDefinitionResolver.h>
#include <sessionManager/actionProtocol/ActionProtocolCompiler.h>
#include <sessionManager/actionProtocol/ActionProtocolAdapter.h>
#include <domain/model/ProviderResult.h>
#include <tools/ZeroWidthEncoder.h>
#include <domain/model/ErrorEvent.h>
#include <drogon/drogon.h>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <sessionManager/core/Session.h>

using namespace drogon;
using namespace provider;
using namespace session;
using namespace error;
using namespace bridge;

namespace {
Json::StreamWriterBuilder& compactJsonWriter() {
    static thread_local Json::StreamWriterBuilder writer = [] {
        Json::StreamWriterBuilder instance;
        instance["indentation"] = "";
        return instance;
    }();
    return writer;
}

std::string toCompactJson(const Json::Value& value) {
    return Json::writeString(compactJsonWriter(), value);
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty() || s.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

void rewriteBridgeConflictsInText(std::string& text) {
    if (text.empty()) return;

    replaceAll(text,
        "Use the provider-native tool-calling mechanism.",
        "Use the text bridge tool-calling format defined by <tool_instructions> for this turn.");

    replaceAll(text,
        "Do not include XML markup or examples.",
        "When a tool call is needed, output ONLY the action format defined by <tool_instructions>.");

    replaceAll(text,
        "provider-native tool-calling mechanism",
        "text bridge tool-calling format defined by <tool_instructions>");

    replaceAll(text,
        "<tool_format>native</tool_format>",
        "<tool_format>text_bridge</tool_format>");

    replaceAll(text,
        "tool_format\":\"native\"",
        "tool_format\":\"text_bridge\"");
}

bool isJsonActionCandidate(const std::string& input, const std::string& sentinel) {
    const size_t sentinelPos = sentinel.empty() ? 0 : input.find(sentinel);
    if (!sentinel.empty() && sentinelPos == std::string::npos) return false;
    size_t pos = sentinelPos + (sentinel.empty() ? 0 : sentinel.size());
    while (pos < input.size() &&
           std::isspace(static_cast<unsigned char>(input[pos]))) {
        ++pos;
    }
    return pos < input.size() && input[pos] == '{';
}

void rewriteBridgeConflictsInMessageContext(Json::Value& messageContext, bool rewriteUserRoleMessages) {
    if (!messageContext.isArray()) return;
    for (auto& msg : messageContext) {
        if (!msg.isObject()) continue;

        const std::string role = msg.get("role", "").asString();
        if (!rewriteUserRoleMessages && role == "user") {
            continue;
        }

        if (msg.isMember("content") && msg["content"].isString()) {
            std::string content = msg["content"].asString();
            rewriteBridgeConflictsInText(content);
            msg["content"] = content;
            continue;
        }

        if (msg.isMember("content") && msg["content"].isArray()) {
            for (auto& part : msg["content"]) {
                if (!part.isObject()) continue;
                if (part.get("type", "").asString() == "text" && part.isMember("text") && part["text"].isString()) {
                    std::string text = part["text"].asString();
                    rewriteBridgeConflictsInText(text);
                    part["text"] = text;
                }
            }
        }
    }
}

void rewriteBridgeConflictingDirectives(session_st& session, bool rewriteUserInput) {
    const size_t beforeSystem = session.request.systemPrompt.size();
    const size_t beforeMessage = session.request.message.size();
    rewriteBridgeConflictsInText(session.request.systemPrompt);

    if (rewriteUserInput) {
        rewriteBridgeConflictsInText(session.request.message);
    }

    rewriteBridgeConflictsInMessageContext(session.provider.messageContext, rewriteUserInput);
    LOG_INFO << "[生成服务] bridge 冲突指令改写已执行: system_len "
              << beforeSystem << "->" << session.request.systemPrompt.size()
              << ", message_len(" << (rewriteUserInput ? "rewritten" : "unchanged_user_input")
              << ") " << beforeMessage << "->" << session.request.message.size();
}

struct ToolChoiceSpec {
    bool none = false;
    bool required = false;
    std::string forcedToolName;
};

bool containsStr(const Json::Value& arr, const std::string& value) {
    if (!arr.isArray() || value.empty()) return false;
    for (const auto& item : arr) {
        if (item.isString() && item.asString() == value) {
            return true;
        }
    }
    return false;
}

ToolChoiceSpec parseToolChoiceSpec(const std::string& toolChoiceRaw) {
    ToolChoiceSpec spec;
    if (toolChoiceRaw.empty()) {
        return spec;
    }

    auto normalizeLower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    if (toolChoiceRaw.front() == '{') {
        Json::Value tc;
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream iss(toolChoiceRaw);
        if (Json::parseFromStream(builder, iss, &tc, &errors) && tc.isObject()) {
            if (tc.get("type", "").asString() == "function" && tc.isMember("function") && tc["function"].isObject()) {
                const auto& function = tc["function"];
                spec.forcedToolName = toolcall::makeBridgeToolName(
                    function.get("namespace", tc.get("namespace", "")).asString(),
                    function.get("name", "").asString());
                if (!spec.forcedToolName.empty()) {
                    spec.required = true;
                }
            }
        }
        return spec;
    }

    const std::string mode = normalizeLower(toolChoiceRaw);
    spec.none = (mode == "none");
    spec.required = (mode == "required");
    return spec;
}

bool isStrictSentinelEnabled(const session_st& session, bool strictToolClient, bool toolChoiceRequired) {
    const auto& customConfig = drogon::app().getCustomConfig();
    if (!customConfig.isObject() || !customConfig.isMember("tool_bridge") || !customConfig["tool_bridge"].isObject()) {
        return strictToolClient || toolChoiceRequired;
    }

    const auto& tb = customConfig["tool_bridge"];
    bool strictSentinel = true;

    if (tb.isMember("strict_sentinel") && tb["strict_sentinel"].isBool()) {
        strictSentinel = tb["strict_sentinel"].asBool();
    }

    if (tb.isMember("strict_sentinel_by_channel") && tb["strict_sentinel_by_channel"].isObject()) {
        const auto& byChannel = tb["strict_sentinel_by_channel"];
        if (byChannel.isMember(session.request.api) && byChannel[session.request.api].isBool()) {
            strictSentinel = byChannel[session.request.api].asBool();
        }
    }

    if (tb.isMember("strict_sentinel_by_model") && tb["strict_sentinel_by_model"].isObject()) {
        const auto& byModel = tb["strict_sentinel_by_model"];
        if (!session.request.model.empty() && byModel.isMember(session.request.model) && byModel[session.request.model].isBool()) {
            strictSentinel = byModel[session.request.model].asBool();
        }
    }

    if (tb.isMember("strict_sentinel_disabled_channels") &&
        containsStr(tb["strict_sentinel_disabled_channels"], session.request.api)) {
        strictSentinel = false;
    }

    if (tb.isMember("strict_sentinel_enabled_channels") &&
        containsStr(tb["strict_sentinel_enabled_channels"], session.request.api)) {
        strictSentinel = true;
    }

    if (tb.isMember("strict_sentinel_disabled_models") &&
        containsStr(tb["strict_sentinel_disabled_models"], session.request.model)) {
        strictSentinel = false;
    }

    if (tb.isMember("strict_sentinel_enabled_models") &&
        containsStr(tb["strict_sentinel_enabled_models"], session.request.model)) {
        strictSentinel = true;
    }

    if (strictToolClient || toolChoiceRequired) {
        strictSentinel = true;
    }

    return strictSentinel;
}

bool isRetoolAgentModel(const std::string& model) {
    return !model.empty() && model.rfind("agent-", 0) == 0;
}

void appendForcedToolRule(std::ostringstream& policy,
                          const std::string& forcedToolName,
                          const std::string& fallbackLine) {
    if (!forcedToolName.empty()) {
        policy << "Required tool for this task: " << forcedToolName << "\n";
    } else {
        policy << fallbackLine;
    }
}

void appendXmlFunctionCallTemplate(std::ostringstream& policy,
                                   const std::string& triggerSignal) {
    policy << triggerSignal << "\n";
    policy << "<function_calls>\n";
    policy << "  <function_call>\n";
    policy << "    <tool>TOOL_NAME</tool>\n";
    policy << "    <args_json><![CDATA[{\"PARAM_NAME\":\"VALUE\"}]]></args_json>\n";
    policy << "  </function_call>\n";
    policy << "</function_calls>\n";
}

void appendRetoolAgentRoutePolicy(std::ostringstream& policy,
                                  const std::string& triggerSignal,
                                  const std::string& forcedToolName) {
    policy << "You are a strict XML tool router.\n\n";
    policy << "Primary rule:\n";
    policy << "- Your entire response must be exactly one XML tool call.\n";
    policy << "- No explanations.\n";
    policy << "- No markdown.\n";
    policy << "- No prose.\n";
    policy << "- No analysis.\n";
    policy << "- No greeting.\n";
    policy << "- No text outside XML.\n";
    policy << "- If you output anything except valid XML in the required format, the response is invalid.\n\n";
    policy << "Decision rule:\n";
    policy << "- If a tool is needed, output exactly one tool call.\n";
    policy << "- If no tool is needed, output exactly one attempt_completion call.\n\n";
    appendForcedToolRule(policy, forcedToolName, "Output exactly one function_call only.\n");
    policy << "\nMandatory format:\n";
    appendXmlFunctionCallTemplate(policy, triggerSignal);
    policy << "\nHard constraints:\n";
    policy << "- Exactly one function_call only.\n";
    policy << "- TOOL_NAME must exist in API Definitions.\n";
    policy << "- args_json must be valid JSON.\n";
    policy << "- Include every required argument.\n";
    policy << "- Argument names must exactly match the schema.\n";
    policy << "- Do not summarize the user task.\n";
    policy << "- Do not explain why you selected the tool.\n";
    policy << "- Do not apologize.\n";
    policy << "- Do not mention limitations or permissions.\n";
    policy << "- Do not ask for file contents if file tools can retrieve them.\n\n";
    policy << "Valid example:\n";
    policy << triggerSignal << "\n";
    policy << "<function_calls>\n";
    policy << "  <function_call>\n";
    policy << "    <tool>list_files</tool>\n";
    policy << "    <args_json><![CDATA[{\"path\":\"src\",\"recursive\":true}]]></args_json>\n";
    policy << "  </function_call>\n";
    policy << "</function_calls>\n\n";
    policy << "Invalid example:\n";
    policy << "I will use list_files.\n";
    policy << triggerSignal << "\n";
    policy << "<function_calls>...</function_calls>\n";
    policy << "Reason invalid: contains non-XML text before the tool call.\n";
}

void appendRetoolProviderPolicy(std::ostringstream& policy,
                                const std::string& triggerSignal,
                                const std::string& forcedToolName) {
    policy << "You are a tool router, not a general chat assistant.\n\n";
    policy << "Your job:\n";
    policy << "- Output exactly ONE tool call when a tool is needed.\n";
    policy << "- If no tool is needed, output exactly ONE attempt_completion call.\n";
    policy << "- Output XML ONLY.\n";
    policy << "- Do NOT output explanations.\n";
    policy << "- Do NOT output markdown.\n";
    policy << "- Do NOT repeat the user's request.\n";
    policy << "- Do NOT add any text before or after the XML.\n";
    policy << "- Any extra text is an error.\n\n";
    appendForcedToolRule(policy, forcedToolName, "Output exactly one function_call only.\n");
    policy << "\nRequired output format:\n";
    appendXmlFunctionCallTemplate(policy, triggerSignal);
    policy << "\nRules:\n";
    policy << "- <tool> must be one of the tools defined below.\n";
    policy << "- <args_json> must be valid JSON.\n";
    policy << "- Include all required arguments.\n";
    policy << "- Argument names are case-sensitive.\n";
    policy << "- Output exactly one <function_call>.\n";
    policy << "- If the task is already complete or no tool is needed, use attempt_completion.\n";
    policy << "- Do not ask the user to paste file contents if a file-reading tool can be used.\n";
    policy << "- Prefer direct file/system tools over explanations.\n\n";
    policy << "Valid examples:\n";
    policy << triggerSignal << "\n";
    policy << "<function_calls>\n";
    policy << "  <function_call>\n";
    policy << "    <tool>list_files</tool>\n";
    policy << "    <args_json><![CDATA[{\"path\":\"src\",\"recursive\":true}]]></args_json>\n";
    policy << "  </function_call>\n";
    policy << "</function_calls>\n\n";
    policy << triggerSignal << "\n";
    policy << "<function_calls>\n";
    policy << "  <function_call>\n";
    policy << "    <tool>attempt_completion</tool>\n";
    policy << "    <args_json><![CDATA[{\"result\":\"Task completed successfully.\"}]]></args_json>\n";
    policy << "  </function_call>\n";
    policy << "</function_calls>\n";
}

bool codexRequestLikelyNeedsExternalState(const std::string& message) {
    if (message.empty() || message.find("[tool_result") != std::string::npos) {
        return false;
    }

    std::string lower = message;
    for (auto& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    static const char* kKeywords[] = {
        "file", "directory", "folder", "repository", "repo", "git ",
        "commit", "branch", "log", "workspace", "codebase", "source code",
        "run ", "execute ", "command", "build", "compile", "test",
        "inspect", "check ", "read ", "open ", "list ", "find ", "search ",
        "文件", "目录", "仓库", "代码", "日志", "提交", "分支", "工作区",
        "运行", "执行", "命令", "构建", "编译", "测试", "检查", "查看",
        "读取", "打开", "列出", "查找", "搜索"
    };
    for (const char* keyword : kKeywords) {
        if (lower.find(keyword) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void appendCodexJsonBridgePolicy(std::ostringstream& policy,
                                const std::string& forcedToolName,
                                const std::string& toolChoice,
                                const std::string& triggerSignal,
                                bool parallelToolCalls,
                                bool requireToolForCurrentRequest) {
    // 此处硬编码 "Codex" 是有意为之：本函数专职生成 Codex 家族的 JSON 桥策略，
    // 客户端身份是函数契约的一部分而非运行时输入。仍走 capabilitiesForClient
    // 而不手工拼装能力，是为了让策略文本自动跟随家族矩阵演进。
    const auto capabilities = actionproto::capabilitiesForClient("Codex", parallelToolCalls);
    policy << actionproto::ActionProtocolCompiler::buildRouterPolicy(triggerSignal, capabilities);
    if (!forcedToolName.empty()) {
        policy << "Required tool for this response: " << forcedToolName << ".\n";
    } else if (toolChoice == "required" || requireToolForCurrentRequest) {
        policy << "This request requires external inspection; use a real tool_call, not final_response.\n";
    } else {
        policy << "Use final_response only after all required tool work is complete.\n";
    }
}

std::optional<toolcall::ToolDefinitionMatch> resolveToolDefinition(
    const session_st& session,
    const generation::ToolCallDone& call)
{
    auto findIn = [&](const Json::Value& tools)
        -> std::optional<toolcall::ToolDefinitionMatch> {
        if (!call.namespacePath.empty() && !call.originalName.empty()) {
            auto match = toolcall::findToolDefinition(
                tools, call.namespacePath, call.originalName);
            if (match.has_value()) return match;
        }
        if (!call.name.empty()) {
            auto match = toolcall::findToolDefinition(tools, call.name);
            if (match.has_value()) return match;
        }
        return std::nullopt;
    };

    auto match = findIn(session.request.toolsRaw);
    if (!match.has_value()) match = findIn(session.request.tools);
    return match;
}

void annotateToolCallIdentities(
    const session_st& session,
    std::vector<generation::ToolCallDone>& toolCalls)
{
    for (auto& call : toolCalls) {
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

void appendRetoolChannelSpecialRules(std::ostringstream& policy);

void appendGenericXmlBridgePolicy(std::ostringstream& policy,
                                  bool strictToolClient,
                                  const std::string& forcedToolName,
                                  const std::string& toolChoice,
                                  const std::string& triggerSignal,
                                  const std::string& apiName) {
    if (strictToolClient) {
        policy << "Context: Software engineering collaboration\n";
        policy << "Task: Generate the next tool call instruction (tools are executed by an external system)\n";
        policy << "Goal: Based on the user request, select and output exactly 1 tool to call.\n";
        policy << "Note: Do NOT explain whether you have access/permissions. Do NOT ask the user to paste file contents. Use the listed tools directly (e.g., call read_file/list_files when you need files).\n";
    } else {
        policy << "Context: Software engineering collaboration\n";
        policy << "Task: Generate tool call instructions (tools are executed by an external system)\n";
        policy << "Goal: Based on the user request, select and output one or more tools to call.\n";
        policy << "Note: Do NOT explain whether you have access/permissions. Do NOT ask the user to paste file contents. Use the listed tools directly (e.g., call read_file/list_files when you need files).\n";
    }

    if (!forcedToolName.empty()) {
        policy << "Required tool for this task: " << forcedToolName << "\n";
    } else if (toolChoice == "required") {
        policy << "This task MUST output exactly 1 tool call (choose the most appropriate API).\n";
    } else if (strictToolClient) {
        policy << "Each response MUST output exactly 1 tool call.\n";
        policy << "If no other tool is needed, use attempt_completion to output the final result.\n";
    } else {
        policy << "Output tool calls in XML format ONLY when an API call is needed to complete the task; otherwise respond normally.\n";
    }

    policy << "\nRequirements:\n";
    policy << "- When outputting a tool call, output ONLY the XML (no explanations, no prefixes/suffixes, no markdown code blocks).\n";
    policy << "- You MUST follow this exact XML format (line 1 is the trigger marker, line 2 starts <function_calls>):\n";
    appendXmlFunctionCallTemplate(policy, triggerSignal);
    policy << "- <tool> MUST be a name that exists in the API definitions below.\n";
    policy << "- <args_json> MUST be a valid JSON object containing all required arguments for that tool.\n";
    policy << "- Parameter keys MUST exactly match the API definition (case-sensitive).\n";

    if (apiName == "retoolapi") {
        appendRetoolChannelSpecialRules(policy);
    }
}

void appendRetoolChannelSpecialRules(std::ostringstream& policy) {
    policy << "- Retool channel special rule: Each response MUST output exactly 1 tool call.\n";
    policy << "- If no other tool is needed, use attempt_completion to output the final result.\n";
    policy << "- You are being called by an automated executor. If you output any text outside XML, the task fails immediately.\n";
    policy << "- Do NOT output explanations, plans, reasoning, acknowledgements, apologies, markdown, prefixes, suffixes, or summaries.\n";
    policy << "- Do NOT describe the tool you are about to use.\n";
    policy << "- Do NOT output natural-language prefaces such as 'I need to use write_to_file' or 'Let me update the file now'.\n";
    policy << "- Even if you know the correct tool, do not explain it; directly output the XML tool call.\n";
    policy << "- Wrong example (invalid because it is not XML-only): 我需要使用 write_to_file 工具来更新 README，但我注意到我应该直接执行工具调用。让我现在更新 codex/README.md 文件。\n";
    policy << "- Correct behavior: directly output the write_to_file XML function_call and nothing else.\n";
}

} // 匿名命名空间

/**
 * @brief 将上游原始响应转换为统一事件序列并发送到 sink
 *
 * 【事件组装详细说明】
 * 1. 先做文本清洗与工具调用提取（原生或桥接模式）。
 * 2. 再做参数规范化与校验过滤，避免把不完整调用发送给客户端。
 * 3. 根据客户端规则执行收敛（例如 Roo/Kilo 仅允许单工具调用）。
 * 4. 最后按 ToolCallDone → OutputTextDone → Completed 顺序输出。
 */
void GenerationService::emitResultEvents(const session_st& session, IResponseSink& sink) {
    // ==================== 步骤 0: 初始化 ====================
    // 从 会话.响应. 中提取上游模型返回的原始文本
    std::string text = sanitizeOutput(
        session.provider.clientInfo,
        safeJsonAsString(session.response.message.get("message", ""), "")
    );
    
    // 获取客户端类型，用于后续的客户端特定处理
    const std::string clientType = safeJsonAsString(session.provider.clientInfo.get("client_type", ""), "");
    
    // 客户端能力 IR：本函数内所有客户端差异均由该对象描述，不再比较字符串。
    // 之所以在此处一次性求值并复用，是因为能力是「客户端 × parallelToolCalls」
    // 的函数——同一请求内必须保持一致，分散重算会有取值漂移风险。
    const auto clientCaps = actionproto::capabilitiesForClient(
        clientType, session.request.parallelToolCalls);

    // 判断是否为"严格工具客户端"（Roo/Kilo）
    // 这类客户端要求：每次响应必须且只能包含 1 个工具调用
    // 判定依据是能力 IR 的推导谓词（requiresActionEveryTurn && 有收尾工具），
    // 而非客户端名——新增同类客户端时此处无需改动。
    const bool strictToolClient = clientCaps.isStrictToolClient();

    // ==================== 步骤 2: 工具调用解析 ====================
    // 检查当前通道是否原生支持工具调用
    // 如果支持，上游会直接返回结构化的 tool_calls，无需再从文本中二次解析
    const bool supportsToolCalls = getChannelSupportsToolCalls(session.request.api);
    // Codex 兼容模式：仅当通道**不**原生支持 tool_calls 时启用。
    // 此时上游只会吐纯文本，需要按 Roo 风格从文本里二次解析 action 协议；
    // 若通道原生支持则不应介入，避免对结构化结果做多余的文本解析。
    const bool codexRooCompat =
        clientCaps.family == actionproto::ClientFamily::Codex && !supportsToolCalls;
    
    const ToolChoiceSpec toolChoiceSpec = parseToolChoiceSpec(session.request.toolChoice);
    const bool toolChoiceNone = toolChoiceSpec.none;
    const bool toolChoiceRequired = toolChoiceSpec.required;
    // Roo/Kilo and CodexRooCompat require the exact per-request sentinel.
    const bool strictSentinelEnabled = isStrictSentinelEnabled(
        session,
        strictToolClient || codexRooCompat,
        toolChoiceRequired
    );
    const bool allowFunctionCallsFallback = !strictSentinelEnabled;
    const bool requireBridgeSentinel = strictSentinelEnabled;
    const std::string forcedToolName = toolChoiceSpec.forcedToolName;

    // 用于存储解析结果
    std::string textContent;                              // 普通文本内容
    std::vector<generation::ToolCallDone> toolCalls;      // 解析出的工具调用列表

    // 优先使用上游原生返回的 tool_calls
    if (session.response.message.isMember("tool_calls") && session.response.message["tool_calls"].isArray()) {
        int index = 0;
        for (const auto& tc : session.response.message["tool_calls"]) {
            if (!tc.isObject()) continue;
            generation::ToolCallDone out;
            out.id = tc.get("id", "").asString();
            out.originalName = tc.get("name", "").asString();
            out.namespacePath = tc.get("namespace", "").asString();
            out.name = toolcall::makeBridgeToolName(
                out.namespacePath, out.originalName);
            out.arguments = tc.get("arguments", "{}").asString();
            out.index = index++;
            if (!out.name.empty()) {
                toolCalls.push_back(out);
            }
        }
    }

    // 【解析策略】
    // - 通道原生支持工具调用 → 不再从文本解析，直接使用原始文本
    // - tool_choice= → 显式禁用工具，全部按普通文本处理
    // - 其他情况（桥接 模式）→ 从文本中解析 XML 格式工具调用
    if (!toolCalls.empty()) {
        textContent = std::move(text);
    } else if (supportsToolCalls || toolChoiceNone) {
        // 通道支持原生工具调用或已禁用工具：直接使用原始文本
        textContent = std::move(text);
    } else {
        // ========== Bridge 模式：严格使用请求阶段已固定的 codec ==========
        //
        // 【为什么需要工具桥接模式？】
        // 某些上游通道（如部分 OpenAI 兼容接口）不支持原生 tool_calls，
        // 我们通过在提示词中注入工具定义，让模型以 XML 格式输出工具调用，
        // 再在这里完成 XML 解析并转换为标准 tool_calls 结构。
        
        // 步骤 2.1：提取本次请求 Sentinel 后的协议输入。

        // - 优先查找本次请求的随机触发标记 (会话.toolBridgeTrigger)
        // - 如果找到触发标记，返回从触发标记开始的子串
        // - strict_sentinel=true: 仅匹配触发标记
        // - strict_sentinel=false: 允许退化匹配 <function_calls>
        std::string bridgeInput = extractXmlInputForToolCalls(
            session, text, allowFunctionCallsFallback);

        if (bridgeInput.empty()) {
            // Bridge 转换后 tools 已被清空，因此诊断必须优先读取 toolsRaw。
            const Json::Value& bridgeToolDefs =
                (!session.request.toolsRaw.isNull() &&
                 session.request.toolsRaw.isArray() &&
                 session.request.toolsRaw.size() > 0)
                    ? session.request.toolsRaw
                    : session.request.tools;
            if (bridgeToolDefs.isArray() && bridgeToolDefs.size() > 0) {
                Json::Value detail;
                detail["client_type"] = clientType;
                detail["tool_choice"] = session.request.toolChoice.empty()
                    ? "auto(default)" : session.request.toolChoice;
                detail["strict_sentinel"] = requireBridgeSentinel;
                detail["sentinel_present"] =
                    !session.provider.toolBridgeTrigger.empty() &&
                    text.find(session.provider.toolBridgeTrigger) != std::string::npos;
                detail["function_calls_present"] =
                    text.find("<function_calls>") != std::string::npos;
                detail["function_call_present"] =
                    text.find("<function_call>") != std::string::npos;
                detail["configured_format"] = toolcall::bridgeWireFormatName(
                    session.provider.toolBridgeFormat);
                detail["tool_definition_count"] =
                    static_cast<Json::UInt64>(bridgeToolDefs.size());
                recordWarnStat(
                    session,
                    metrics::Domain::TOOL_BRIDGE,
                    metrics::EventType::TOOLBRIDGE_XML_NOT_FOUND,
                    "响应中未找到本次请求配置格式的工具桥动作",
                    detail,
                    text.substr(0, std::min(text.size(), size_t(1024)))
                );
            }
            // 没有找到当前请求的协议动作，全部按普通文本处理。
            textContent = std::move(text);
        } else {
            bridgeInput = normalizeBridgeXml(std::move(bridgeInput));

            toolcall::BridgePolicyOptions decodeOptions;
            decodeOptions.clientType = clientType;
            decodeOptions.channel = session.request.api;
            decodeOptions.model = session.request.model;
            decodeOptions.sentinel = session.provider.toolBridgeTrigger;
            decodeOptions.parallelToolCalls = session.request.parallelToolCalls;

            auto selectedCodec = toolcall::createBridgeProtocolCodec(
                session.provider.toolBridgeFormat);
            auto decoded = selectedCodec->decodeResponse(bridgeInput, decodeOptions);

            // 仅在显式配置迁移回退时尝试另一个格式。默认严格禁止跨格式解析。
            if ((!decoded.matched || !decoded.valid) &&
                session.provider.toolBridgeAllowFormatFallback) {
                const auto fallbackFormat = session.provider.toolBridgeFormat ==
                    toolcall::BridgeWireFormat::Json
                        ? toolcall::BridgeWireFormat::Xml
                        : toolcall::BridgeWireFormat::Json;
                auto fallbackCodec = toolcall::createBridgeProtocolCodec(fallbackFormat);
                auto fallbackDecoded = fallbackCodec->decodeResponse(
                    bridgeInput, decodeOptions);
                if (fallbackDecoded.matched && fallbackDecoded.valid) {
                    LOG_WARN << "[生成服务][ToolBridge] 响应使用兼容格式解析: configured="
                             << toolcall::bridgeWireFormatName(
                                    session.provider.toolBridgeFormat)
                             << ", detected=" << fallbackDecoded.protocol;
                    decoded = std::move(fallbackDecoded);
                }
            }

            if (!decoded.valid) {
                Json::Value detail;
                detail["configured_format"] = toolcall::bridgeWireFormatName(
                    session.provider.toolBridgeFormat);
                detail["detected_protocol"] = decoded.protocol;
                detail["matched"] = decoded.matched;
                detail["error_code"] = static_cast<int>(decoded.diagnostic.code);
                detail["offset"] = static_cast<Json::UInt64>(
                    decoded.diagnostic.offset);
                recordWarnStat(
                    session,
                    metrics::Domain::TOOL_BRIDGE,
                    metrics::EventType::TOOLBRIDGE_PARSE_ERROR,
                    "固定格式动作协议解析失败: " + decoded.diagnostic.message,
                    detail,
                    text.substr(0, std::min(text.size(), size_t(2048)))
                );
                textContent = text;
            } else {
                LOG_INFO << "[生成服务][ToolBridge] 响应解析成功: configured="
                          << toolcall::bridgeWireFormatName(
                                 session.provider.toolBridgeFormat)
                          << ", detected=" << decoded.protocol;
                toolCalls = std::move(decoded.toolCalls);
                textContent = std::move(decoded.text);
            }

            if (requireBridgeSentinel && bridgeInput.find(
                    session.provider.toolBridgeTrigger) == std::string::npos) {
                toolCalls.clear();
                textContent = std::move(text);
            }
        }

        // 步骤 2.5: 强制工具调用兜底
        // 如果 tool_choice=required 但上游未返回工具调用，
        // 尝试根据用户输入自动生成一个工具调用
        if (toolCalls.empty()) {
            toolcall::generateForcedToolCall(session, toolCalls, textContent);
        }
    }

    // ==================== 步骤 3: tool_choice 强制工具名约束 ====================
    annotateToolCallIdentities(session, toolCalls);

    if (!forcedToolName.empty() && !toolCalls.empty()) {
        std::vector<generation::ToolCallDone> filteredCalls;
        filteredCalls.reserve(toolCalls.size());
        std::vector<std::string> rejectedNames;

        for (const auto& tc : toolCalls) {
            if (tc.name == forcedToolName) {
                filteredCalls.push_back(tc);
            } else {
                rejectedNames.push_back(tc.name);
            }
        }

        if (!rejectedNames.empty()) {
            std::ostringstream rejected;
            for (size_t i = 0; i < rejectedNames.size(); ++i) {
                if (i) rejected << ",";
                rejected << rejectedNames[i];
            }

            Json::Value detail;
            detail["forced_tool_name"] = forcedToolName;
            detail["rejected_count"] = static_cast<Json::UInt64>(rejectedNames.size());
            detail["rejected_tools"] = rejected.str();
            recordWarnStat(
                session,
                metrics::Domain::TOOL_BRIDGE,
                metrics::EventType::TOOLBRIDGE_VALIDATION_FILTERED,
                "tool_choice 指定函数约束生效，已过滤非指定工具调用",
                detail
            );
        }

        toolCalls = std::move(filteredCalls);
    }

    if (codexRooCompat && !toolCalls.empty()) {
        std::vector<generation::ToolCallDone> executableCalls;
        executableCalls.reserve(toolCalls.size());
        std::string completionText;

        for (const auto& tc : toolCalls) {
            if (tc.name != "attempt_completion") {
                executableCalls.push_back(tc);
                continue;
            }

            Json::Value args;
            Json::CharReaderBuilder builder;
            std::string errors;
            std::istringstream iss(tc.arguments);
            if (Json::parseFromStream(builder, iss, &args, &errors) &&
                args.isObject() && args.isMember("result") &&
                args["result"].isString() && !args["result"].asString().empty() &&
                completionText.empty()) {
                completionText = args["result"].asString();
            } else {
                LOG_WARN << "[生成服务][CodexRooCompat] attempt_completion 参数无效";
            }
        }

        if (!executableCalls.empty()) {
            toolCalls = std::move(executableCalls);
            textContent.clear();
        } else if (!completionText.empty()) {
            toolCalls.clear();
            textContent = completionText;
            LOG_INFO << "[生成服务][CodexRooCompat] 已将虚拟 attempt_completion 转换为最终文本";
        } else {
            toolCalls.clear();
        }
    }

    if (toolCalls.empty() && toolChoiceRequired) {
        toolcall::generateForcedToolCall(session, toolCalls, textContent);
    }

    // ==================== 步骤 4: 参数形状规范化 ====================
    // 规范化ToolCallArguments() 的作用：
    // - 修复常见的参数格式问题（如 ：[""] → ：[{：""}]）
    // - 处理参数别名（如 → ）
    // - 填充缺失的可选字段默认值
    // - 规范化 ask_followup_question 的 字段
    annotateToolCallIdentities(session, toolCalls);
    toolcall::normalizeToolCallArguments(session, toolCalls);

    // ==================== 步骤 4： 校验与无效调用过滤 ====================
    // 【这是防止“缺少 nativeArgs”错误的关键步骤】
    //
    // 【问题背景】
    // 当上游模型输出不完整的工具调用（如参数 JSON 截断、必填字段缺失）时，
    // 客户端执行层会报错“无效 工具调用： nativeArgs”。
    //
    // 【解决方案】
    // 在发送给客户端之前，使用 ToolCallValidator 进行分层校验：
    // - 阶段A：工具名存在性 + JSON 可解析 + 必填字段 + 类型匹配
    // - 阶段B：关键字段非空（// 等）
    // - 阶段C：校验失败后的降级策略（按客户端类型）
    {
        // 获取工具定义（优先使用 toolsRaw，它保存了原始的客户端工具定义）
        const Json::Value& toolDefs =
            (!session.request.toolsRaw.isNull() && session.request.toolsRaw.isArray() && session.request.toolsRaw.size() > 0)
                ? session.request.toolsRaw
                : session.request.tools;
        
        // 只有在有工具调用且有工具定义时才进行校验
        if (!toolCalls.empty() && toolDefs.isArray() && toolDefs.size() > 0) {
            // 创建校验器，传入工具定义和客户端类型
            // 客户端类型用于选择不同的关键字段集合：
            // - RooCode/Kilo-Code： 使用完整的关键字段集合
            // - 其他客户端: 使用最小关键字段集合
            toolcall::ToolCallValidator validator(toolDefs, clientType);
            
            // 用于收集被丢弃的工具调用信息（供降级策略使用）
            std::string discardedText;
            
            // 【校验模式选择】
            // 根据客户端类型自动选择推荐的校验模式：
            // - RooCode/Kilo-Code： 模式（校验关键字段）
            // - 其他客户端： 模式（不校验，信任 AI 输出）
            //
            // 原因：
            // 1. Roo/Kilo 客户端对工具调用格式要求严格，需要提前过滤明显错误
            // 2. 其他客户端使用宽松策略，避免误报（如 字段问题）
            // 3. 提示词中已经明确告诉 AI 哪些参数是 必填
            toolcall::ValidationMode validationMode = toolcall::getRecommendedValidationMode(clientType);
            
            // 执行校验并过滤无效的工具调用

            // - 遍历所有工具调用，逐个校验
            // - 移除不通过校验的工具调用
            // - 返回被移除的数量
            size_t removedCount = validator.filterInvalidToolCalls(toolCalls, discardedText, validationMode);
            
            if (removedCount > 0) {
                LOG_WARN << "[生成服务] 通过 校验过滤了" << removedCount
                         << " 个无效的工具调用";
                
                // 记录 TOOL_BRIDGE 警告：校验过滤了无效的工具调用
                Json::Value filterDetail;
                filterDetail["removed_count"] = static_cast<Json::UInt64>(removedCount);
                filterDetail["validation_mode"] = static_cast<int>(validationMode);
                recordWarnStat(
                    session,
                    metrics::Domain::TOOL_BRIDGE,
                    metrics::EventType::TOOLBRIDGE_VALIDATION_FILTERED,
                    "已过滤无效工具调用数量: " + std::to_string(removedCount),
                    filterDetail,
                    discardedText.substr(0, std::min(discardedText.size(), size_t(2048)))
                );
                
                // 阶段C：根据客户端类型应用降级策略
                // 如果所有工具调用都被过滤掉了，需要决定如何处理
                if (toolCalls.empty()) {

                    // - 非严格客户端：仅丢弃，保留文本输出
                    // - 严格客户端（Roo/Kilo）：将文本包装为 attempt_completion
                    toolcall::applyValidationFallback(clientType, toolCalls, textContent, discardedText);
                    
                    // 记录 TOOL_BRIDGE 警告：应用了降级策略
                    recordWarnStat(
                        session,
                        metrics::Domain::TOOL_BRIDGE,
                        metrics::EventType::TOOLBRIDGE_VALIDATION_FALLBACK_APPLIED,
                        "已应用校验降级策略，客户端: " + clientType
                    );
                }
            }
        }
    }

    // ==================== 步骤 5： 严格客户端规则（仅 Roo/Kilo）====================
    // 【规则说明】
    // Roo/Kilo 客户端要求每次响应必须且只能包含 1 个工具调用。
    //
    // applyStrict客户端Rules() 的行为：
    // - 如果没有工具调用但有文本 → 包装为 attempt_completion
    // - 如果有多个工具调用 → 只保留第一个
    // - 如果有工具调用 → 清空文本内容（避免重复输出）
    if (strictToolClient) {
        // 记录应用严格客户端规则前的状态
        size_t toolCallsBeforeStrict = toolCalls.size();
        bool hadTextBeforeStrict = !textContent.empty();
        
        toolcall::applyStrictClientRules(clientType, textContent, toolCalls);
        
        // 如果规则导致了变化，记录统计
        if (toolCallsBeforeStrict != toolCalls.size() ||
            (hadTextBeforeStrict && textContent.empty() && !toolCalls.empty())) {
            Json::Value strictDetail;
            strictDetail["original_tool_calls"] = static_cast<Json::UInt64>(toolCallsBeforeStrict);
            strictDetail["final_tool_calls"] = static_cast<Json::UInt64>(toolCalls.size());
            strictDetail["text_cleared"] = hadTextBeforeStrict && textContent.empty();
            recordWarnStat(
                session,
                metrics::Domain::TOOL_BRIDGE,
                metrics::EventType::TOOLBRIDGE_STRICT_CLIENT_RULE_APPLIED,
                "已应用严格客户端规则，客户端: " + clientType,
                strictDetail
            );
        }
    }

    // ---- 并行工具调用收敛 ----
    // 上限来自能力 IR（Codex 在 parallel_tool_calls=false 时为 1），
    // 不再对客户端名做硬编码判断，因此该规则天然适用于任何声明了
    // coalescesParallelCalls 的家族。
    // 策略选择「截断」而非「报错」：多余调用丢弃后客户端仍可在下一轮补做，
    // 而报错会直接中断会话，代价明显更高。
    // 截断后紧接着重排 index，保证下发的 tool_call 索引连续从 0 起。
    if (clientCaps.coalescesParallelCalls &&
        toolCalls.size() > clientCaps.maxToolCalls) {
        LOG_WARN << "[生成服务][ToolBridge] 客户端能力限制 maxToolCalls="
                 << clientCaps.maxToolCalls << "，已截断多余工具调用";
        toolCalls.erase(toolCalls.begin() + clientCaps.maxToolCalls, toolCalls.end());
    }
    for (size_t i = 0; i < toolCalls.size(); ++i) {
        toolCalls[i].index = static_cast<int>(i);
    }

    // ==================== 步骤 7: 零宽字符会话ID嵌入 ====================
    // 【功能说明】
    // 使用零宽字符将会话ID嵌入到响应文本中，客户端下次请求时可以提取该ID实现续聊。
    //
    // 【两阶段设计】
    // 1. prepareNext会话Id() 在 emitResultEvents 之前预生成 next会话Id（候选会话ID）
    // 2. 这里优先嵌入 next会话Id（而非 conversationId）
    // 3. commit会话Transfer() 在发送后执行真正的会话迁移
    //
    // 【特殊处理】
    // 对于 tool_calls 场景，部分客户端在收到 finish_reason="tool_calls" 后
    // 会停止处理后续内容，因此必须在 tool_calls 事件前发送会话ID。
    auto& sessionManager = *sessionStore_;

    // 优先使用预生成的 next会话Id；若为空则回退到 conversationId
    const std::string& sessionIdToEmbed =
        !session.state.nextSessionId.empty() ? session.state.nextSessionId : session.state.conversationId;

    const bool usesStableClientSession =
        continuity::hasStableClientSession(session.provider.clientInfo);
    if (sessionManager.isZeroWidthMode() &&
        !usesStableClientSession &&
        !sessionIdToEmbed.empty()) {
        if (!toolCalls.empty() && clientCaps.stopsConsumingTextAfterToolCall) {
            // 工具客户端可能在 tool_calls 后停止消费文本，因此先发送仅含零宽 ID 的文本事件。
            // 该分支由能力位驱动（Codex / ClaudeCode 置位），而非客户端名比较：
            // 若不前置，会话 ID 将随被丢弃的尾部文本一起丢失，导致下一轮无法续聊。
            // 这里发出的是「仅零宽字符」的文本事件，对用户可见内容无任何影响。
            std::string zwOnly = chatSession::embedSessionIdInText("", sessionIdToEmbed);
            if (!zwOnly.empty()) {
                generation::OutputTextDone zwDone;
                zwDone.text = zwOnly;
                zwDone.index = 0;
                sink.onEvent(zwDone);
                LOG_INFO << "[生成服务] 已在 tool_calls 事件前发送零宽会话ID：" << sessionIdToEmbed
                         << "（当前会话: " << session.state.conversationId << ")";
            }
        } else {
            // 普通文本回复在正文末尾嵌入会话 ID。
            textContent = chatSession::embedSessionIdInText(textContent, sessionIdToEmbed);
            LOG_INFO << "[生成服务] 已在响应中嵌入会话ID: " << sessionIdToEmbed
                     << "（当前会话: " << session.state.conversationId << ")";
        }
    }

    // ==================== 步骤 8: 发送事件 ====================
    // 【事件发送顺序】
    // 1. ToolCallDone 事件（如果有工具调用）
    // 2. OutputTextDone 事件（如果有文本内容）
    // 3. 已完成 事件（标记响应结束）
    
    // 发送所有工具调用事件
    for (const auto& tc : toolCalls) {
        sink.onEvent(tc);
    }

    // 发送文本内容事件（如果有）
    if (!textContent.empty()) {
        generation::OutputTextDone textDone;
        textDone.text = textContent;
        textDone.index = 0;
        sink.onEvent(textDone);
    }

    // 发送完成事件
    // finish_reason： "停止"（普通文本结束）或 "tool_calls"（工具调用结束）
    generation::Completed completed;
    completed.finishReason = toolCalls.empty() ? "stop" : "tool_calls";
    if (session.response.message.isMember("_meta") && session.response.message["_meta"].isObject()) {
        completed.meta = session.response.message["_meta"];
    }
    sink.onEvent(completed);
}

void GenerationService::emitError(
    generation::ErrorCode code,
    const std::string& message,
    IResponseSink& sink
) {
    generation::Error error;
    error.code = code;
    error.message = message;
    sink.onEvent(error);
}

std::string GenerationService::sanitizeOutput(
    const Json::Value& clientInfo,
    const std::string& text
) {
    return ClientOutputSanitizer::sanitize(clientInfo, text);
}

bool GenerationService::getChannelSupportsToolCalls(const std::string& channelName) const {
    // ： 从 渠道Manager 内存缓存获取，避免每次请求查数据库
    auto result = channelCatalog_
        ? channelCatalog_->supportsToolCalls(channelName)
        : std::optional<bool>{};
    if (result.has_value()) {
        LOG_INFO << "[生成服务] 通道 " << channelName
                  << " supportsToolCalls: " << result.value();
        return result.value();
    }
    
    // 默认返回 （保守策略，避免破坏现有行为）
    LOG_WARN << "[生成服务] 未找到通道 " << channelName << "，默认支持 工具调用";
    return true;
}

// ========== emitResultEvents 辅助函数实现 ==========

/**
 * @brief 解析 XML 格式的工具调用
 *
 * 【功能说明】
 * 从 XML 文本中解析出工具调用信息。这是 Bridge 模式的核心解析函数。
 *
 * 【XML 格式示例】
 * <Function_Ab1c_Start/>          ← 触发标记（Sentinel）
 * <function_calls>
 *   <function_call>
 *     <tool>read_file</tool>
 *     <args_json><![CDATA[{"path":"src/main.cpp"}]]></args_json>
 *   </function_call>
 * </function_calls>
 *
 * 【Sentinel 机制】
 * - 每次请求生成唯一的触发标记（如 <Function_Ab1c_Start/>）
 * - 只有包含该标记的 XML 块才会被解析
 * - 防止误解析历史消息或示例中的 XML
 *
 * @param xmlInput 待解析的 XML 文本
 * @param outTextContent [输出] 非工具调用的普通文本
 * @param outToolCalls [输出] 解析出的工具调用列表
 * @param sentinel 期望的触发标记，为空则不做严格匹配
 */
void GenerationService::parseXmlToolCalls(
    const std::string& xmlInput,
    std::string& outTextContent,
    std::vector<generation::ToolCallDone>& outToolCalls,
    const std::string& sentinel
) {
    LOG_INFO << "[生成服务] 解析 XML 格式工具调用，触发标记：" << (sentinel.empty() ? "无" : sentinel);
    LOG_INFO << "[生成服务] 输入长度=" << xmlInput.size()
              << "，包含 </args_json>=" << (xmlInput.find("</args_json>") != std::string::npos)
              << "，包含 </function_call>=" << (xmlInput.find("</function_call>") != std::string::npos)
              << "，包含 </function_calls>=" << (xmlInput.find("</function_calls>") != std::string::npos);


    // - ToolCallBridge： 负责协调请求/响应的转换
    // - XmlTagToolCallCodec： 负责 XML 格式的编解码
    auto bridge = toolcall::createToolCallBridge(false);
    auto codec = toolcall::createXmlTagToolCallCodec();

    // 设置 （触发标记）
    // 如果设置了 ， 只会解析包含该标记的 XML 块
    if (!sentinel.empty()) {
        codec->setSentinel(sentinel);
    }
    bridge->setTextCodec(codec);

    // 执行解析
    std::vector<toolcall::ToolCallEvent> events;
    bridge->transformResponseChunk(xmlInput, events);  // 增量解析
    bridge->flushResponse(events);                      // 刷新剩余内容

    // 处理解析事件
    for (const auto& event : events) {
        switch (event.type) {
            case toolcall::EventType::Text:
                // 普通文本事件：累积到 outTextContent
                outTextContent += event.text;
                break;
                
            case toolcall::EventType::ToolCallEnd: {
                // 工具调用结束事件：创建 ToolCallDone 结构
                LOG_INFO << "[生成服务] 工具调用结束: " << event.toolName;
                generation::ToolCallDone tc;
                tc.id = event.toolCallId;
                tc.name = event.toolName;
                tc.arguments = event.argumentsDelta;
                tc.index = outToolCalls.size();     // 在列表中的索引
                outToolCalls.push_back(tc);
                break;
            }
            
            case toolcall::EventType::ToolCallBegin:
            case toolcall::EventType::ToolCallArgsDelta:
                // 这些是流式解析的中间状态，非流式场景下忽略
                break;
                
            case toolcall::EventType::Error:
                // 解析错误：记录日志但不中断处理
                // XML and parser text can contain model output, tool args, or
                // user-controlled strings.  Preserve only bounded metadata.
                LOG_WARN << "[生成服务] 工具调用解析错误: errorPresent="
                         << !event.errorMessage.empty()
                         << ", errorSize=" << event.errorMessage.size()
                         << ", inputLength=" << xmlInput.size();
                break;
        }
    }

    // 【重要】如果解析出了工具调用，清空文本内容
    // 避免触发标记、空白字符等被当作普通文本输出
    if (!outToolCalls.empty()) {
        outTextContent.clear();
    }
}

void toolcall::generateForcedToolCall(
    const session_st& session,
    std::vector<generation::ToolCallDone>& outToolCalls,
    std::string& outTextContent
) {
    std::string toolChoice = "auto";
    std::string forcedToolName;

    if (!session.request.toolChoice.empty()) {
        if (session.request.toolChoice.front() == '{') {
            Json::Value tc;
            Json::CharReaderBuilder builder;
            std::string errors;
            std::istringstream iss(session.request.toolChoice);
            if (Json::parseFromStream(builder, iss, &tc, &errors) && tc.isObject()) {
                if (tc.get("type", "").asString() == "function" && tc.isMember("function") && tc["function"].isObject()) {
                    const auto& function = tc["function"];
                    forcedToolName = toolcall::makeBridgeToolName(
                        function.get("namespace", tc.get("namespace", "")).asString(),
                        function.get("name", "").asString());
                    if (!forcedToolName.empty()) {
                        toolChoice = "required";
                    }
                }
            }
        } else {
            toolChoice = toLowerStr(session.request.toolChoice);
        }
    }

    const bool mustCallTool = (toolChoice == "required") || !forcedToolName.empty();
    const Json::Value& toolDefs =
        (!session.request.toolsRaw.isNull() && session.request.toolsRaw.isArray() && session.request.toolsRaw.size() > 0)
            ? session.request.toolsRaw
            : session.request.tools;

    if (!mustCallTool || !toolDefs.isArray() || toolDefs.size() == 0) {
        return;
    }

    auto startsWith = [](const std::string& s, const std::string& prefix) -> bool {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    };
    auto endsWith = [](const std::string& s, const std::string& suffix) -> bool {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    auto stripTrailingPunct = [&](std::string s) -> std::string {
        s = trimWhitespace(std::move(s));
        static const std::vector<std::string> suffixes = {
            "？", "?", "！", "!", "。", ".", "，", ",", "；", ";", ":", "："
        };
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& suf : suffixes) {
                if (endsWith(s, suf)) {
                    s.erase(s.size() - suf.size());
                    s = trimWhitespace(std::move(s));
                    changed = true;
                }
            }
        }
        return s;
    };
    auto takeLastToken = [&](std::string s) -> std::string {
        s = trimWhitespace(std::move(s));
        size_t pos = s.find_last_of(" \t\r\n");
        if (pos == std::string::npos) return s;
        return trimWhitespace(s.substr(pos + 1));
    };

    auto extractLocationLike = [&](std::string input) -> std::string {
        std::string s = stripTrailingPunct(trimWhitespace(std::move(input)));

        // 中文天气表达模式：例如“……的天气……”
        size_t pos = s.find("天气");
        if (pos == std::string::npos) pos = s.find("气温");
        if (pos != std::string::npos) {
            std::string before = stripTrailingPunct(s.substr(0, pos));

            // 移除常见前缀词
            static const std::vector<std::string> prefixes = {
                "请帮我查一下", "请帮我查下", "请帮我查查", "请帮我查", "帮我查一下", "帮我查下", "帮我查查", "帮我查",
                "麻烦查一下", "麻烦查下", "麻烦查查", "麻烦查", "查一下", "查下", "查查", "查询", "看看", "看一下", "请问", "问一下"
            };
            before = trimWhitespace(std::move(before));
            bool removed = true;
            while (removed) {
                removed = false;
                for (const auto& p : prefixes) {
                    if (startsWith(before, p)) {
                        before = trimWhitespace(before.substr(p.size()));
                        removed = true;
                        break;
                    }
                }
            }

            // 移除结尾语气词与时间词
            static const std::vector<std::string> timeSuffixes = {"今天", "明天", "后天", "现在", "当前", "目前"};
            before = stripTrailingPunct(trimWhitespace(std::move(before)));
            for (const auto& t : timeSuffixes) {
                if (endsWith(before, t)) {
                    before.erase(before.size() - t.size());
                    before = trimWhitespace(std::move(before));
                }
            }

            if (endsWith(before, "的")) {
                before.erase(before.size() - std::string("的").size());
                before = trimWhitespace(std::move(before));
            }

            before = takeLastToken(std::move(before));
            if (!before.empty()) return before;
        }

        // 英文天气表达模式：" "
        std::string lower = toLowerStr(s);
        const std::string needle = "weather in ";
        size_t p = lower.find(needle);
        if (p != std::string::npos) {
            std::string loc = stripTrailingPunct(trimWhitespace(s.substr(p + needle.size())));
            if (!loc.empty()) return loc;
        }

        return stripTrailingPunct(trimWhitespace(std::move(s)));
    };

    std::optional<toolcall::ToolDefinitionMatch> selected;
    if (!forcedToolName.empty()) {
        selected = toolcall::findToolDefinition(toolDefs, forcedToolName);
    }
    if (!selected.has_value()) {
        selected = toolcall::firstToolDefinition(toolDefs);
    }
    if (!selected.has_value()) {
        return;
    }
    const std::string toolName = selected->bridgeName;

    Json::Value args(Json::objectValue);
    const std::string srcText = session.request.rawMessage.empty() ? session.request.message : session.request.rawMessage;

    std::vector<std::string> requiredParams;
    if (const Json::Value* schema = toolcall::toolParametersSchema(*selected)) {
        const auto& required = (*schema)["required"];
        if (required.isArray()) {
            for (const auto& r : required) {
                if (r.isString()) requiredParams.push_back(r.asString());
            }
        }
        if (requiredParams.empty() &&
            schema->isMember("properties") &&
            (*schema)["properties"].isObject()) {
            for (const auto& p : (*schema)["properties"].getMemberNames()) {
                requiredParams.push_back(p);
            }
        }
    }

    auto isLocationParam = [&](const std::string& name) -> bool {
        const std::string n = toLowerStr(name);
        return n.find("location") != std::string::npos ||
               n.find("city") != std::string::npos ||
               n.find("place") != std::string::npos ||
               n.find("address") != std::string::npos ||
               n.find("region") != std::string::npos;
    };

    for (const auto& p : requiredParams) {
        if (p.empty()) continue;
        std::string v;
        if (isLocationParam(p)) {
            v = extractLocationLike(srcText);
        } else {
            v = trimWhitespace(srcText);
        }
        args[p] = v;
    }

    generation::ToolCallDone tc;
    tc.id = generateFallbackToolCallId();
    tc.name = toolName;
    tc.originalName = selected->originalName;
    tc.namespacePath = selected->namespacePath;
    tc.type = selected->type;
    tc.index = 0;
    tc.arguments = toCompactJson(args);

    outToolCalls.push_back(tc);
    outTextContent.clear();

    LOG_WARN << "[生成服务] 上游未返回工具调用，已根据 tool_choice=required 生成兜底工具调用：" << toolName;
    
    // 记录 TOOL_BRIDGE 警告：生成了强制工具调用
    Json::Value forcedDetail;
    forcedDetail["tool_name"] = toolName;
    forcedDetail["tool_choice"] = session.request.toolChoice;
    forcedDetail["generated_args"] = args;
    recordWarnStat(
        session,
        metrics::Domain::TOOL_BRIDGE,
        metrics::EventType::TOOLBRIDGE_FORCED_TOOLCALL_GENERATED,
        "已生成强制工具调用: " + toolName,
        forcedDetail,
        srcText.substr(0, std::min(srcText.size(), size_t(512))),
        toolName
    );
}

/**
 * @brief 规范化工具调用参数的形状
 *
 * 【功能说明】
 * 上游模型输出的参数格式可能与客户端期望的 JSONSchema 不完全匹配。
 * 此函数负责将参数转换为正确的形状，避免客户端解析失败。
 *
 * 【常见问题示例】
 * 1. 数组元素类型错误：
 *    - 上游输出: files: ["src/main.cpp", "src/util.cpp"]
 *    - 期望格式: files: [{path: "src/main.cpp"}, {path: "src/util.cpp"}]
 *
 * 2. 参数别名：
 *    - 上游输出: paths: ["src/main.cpp"]
 *    - 期望格式: files: [{path: "src/main.cpp"}]
 *
 * 3. 缺失可选字段：
 *    - 上游输出: follow_up: [{text: "Yes"}]
 *    - 期望格式: follow_up: [{text: "Yes", mode: ""}]
 *
 * @param session 会话状态，包含工具定义
 * @param toolCalls [输入/输出] 待规范化的工具调用列表
 */
void toolcall::normalizeToolCallArguments(
    const session_st& session,
    std::vector<generation::ToolCallDone>& toolCalls
) {
    if (toolCalls.empty()) {
        return;
    }

    // 安全提取 JSON 的 字段（可能是字符串或数组，例如 [""，""]）
    auto safeGetType = [](const Json::Value& schema) -> std::string {
        if (!schema.isObject() || !schema.isMember("type")) return "";
        const auto& typeVal = schema["type"];
        if (typeVal.isString()) return typeVal.asString();
        if (typeVal.isArray()) {
            for (const auto& t : typeVal) {
                if (t.isString() && t.asString() != "null") return t.asString();
            }
            if (typeVal.size() > 0 && typeVal[0].isString()) return typeVal[0].asString();
        }
        return "";
    };

    const Json::Value& toolDefs =
        (!session.request.toolsRaw.isNull() && session.request.toolsRaw.isArray() && session.request.toolsRaw.size() > 0)
            ? session.request.toolsRaw
            : session.request.tools;

    auto findToolObj = [&](const std::string& toolName) -> const Json::Value* {
        const auto match = toolcall::findToolDefinition(toolDefs, toolName);
        return match.has_value() ? match->tool : nullptr;
    };

    auto normalizeArrayOfObjectParam = [&](Json::Value& args, const std::string& paramName, const Json::Value& paramSchema) {
        if (!args.isObject() || !args.isMember(paramName) || !args[paramName].isArray()) {
            return;
        }
        if (!paramSchema.isObject() || safeGetType(paramSchema) != "array") {
            return;
        }
        const auto& items = paramSchema["items"];
        if (!items.isObject() || safeGetType(items) != "object") {
            return;
        }

        std::vector<std::string> requiredKeys;
        const auto& req = items["required"];
        if (req.isArray()) {
            for (const auto& r : req) {
                if (r.isString()) requiredKeys.push_back(r.asString());
            }
        }
        // 未声明 required 时不猜测必填字段，避免误删只包含可选属性的元素。
        if (requiredKeys.empty()) {
            return;
        }

        const Json::Value* itemProps = nullptr;
        if (items.isMember("properties") && items["properties"].isObject()) {
            itemProps = &items["properties"];
        }
        auto getPropType = [&](const std::string& key) -> std::string {
            if (!itemProps || !itemProps->isObject()) return "";
            if (!itemProps->isMember(key) || !(*itemProps)[key].isObject()) return "";
            return safeGetType((*itemProps)[key]);
        };
        auto hasKey = [&](const std::string& key) -> bool {
            for (const auto& k : requiredKeys) {
                if (k == key) return true;
            }
            return false;
        };
        auto pickStringKey = [&]() -> std::string {
            // 上游常见的嵌套数组输出模式：


            if (paramName == "files") return "path";
            if (hasKey("text")) return "text";
            if (hasKey("path")) return "path";
            if (hasKey("name")) return "name";
            if (hasKey("id")) return "id";
            return requiredKeys[0];
        };

        const Json::ArrayIndex originalSize = args[paramName].size();
        Json::Value out(Json::arrayValue);
        for (const auto& el : args[paramName]) {
            Json::Value obj;
            if (el.isString()) {
                obj = Json::Value(Json::objectValue);
                obj[pickStringKey()] = el.asString();
            } else if (el.isObject()) {
                obj = el;
            } else {
                continue;
            }

            // 当必填字段缺失时补齐安全默认值
            for (const auto& k : requiredKeys) {
                if (!obj.isMember(k)) {
                    if (k == "mode") {
                        obj[k] = "";
                    }
                }
            }

            // 特殊处理：read_file 的文件对象期望结构为 {：}
            if (paramName == "files") {
                if (!obj.isMember("path")) {
                    if (obj.isMember("file") && obj["file"].isString()) obj["path"] = obj["file"].asString();
                    else if (obj.isMember("name") && obj["name"].isString()) obj["path"] = obj["name"].asString();
                }
            }

            // 移除无效或不完整条目，避免客户端因 `` 路径等问题崩溃。
            bool valid = true;
            for (const auto& k : requiredKeys) {
                if (k == "mode") {
                    if (!obj.isMember(k) || !obj[k].isString()) obj[k] = "";
                    continue;
                }
                if (!obj.isMember(k) || obj[k].isNull()) {
                    valid = false;
                    break;
                }
                const std::string t = getPropType(k);
                if (t == "string" && !obj[k].isString()) {
                    obj[k] = toCompactJson(obj[k]);
                }
            }

            if (valid && paramName == "files") {
                if (!obj.isMember("path") || !obj["path"].isString() || obj["path"].asString().empty()) {
                    valid = false;
                }
            }

            if (valid) {
                out.append(obj);
            }
        }

        // 规范化不能静默丢失元素，否则非空输入可能被改写成 actions: []。
        // 保留原始非法值交给后续 schema 校验器拒绝并触发降级。
        if (out.size() != originalSize) {
            LOG_WARN << "[生成服务] 数组参数规范化未完成，保留原始参数: "
                     << paramName << ", original=" << originalSize
                     << ", normalized=" << out.size();
            return;
        }
        if (paramSchema["minItems"].isIntegral() &&
            out.size() < paramSchema["minItems"].asUInt()) {
            LOG_WARN << "[生成服务] 数组参数规范化结果不满足 minItems，保留原始参数: "
                     << paramName;
            return;
        }
        args[paramName] = out;
    };

    for (auto& tc : toolCalls) {
        Json::Value args;
        Json::CharReaderBuilder builder;
        std::string errors;
        if (tc.arguments.empty()) {
            args = Json::Value(Json::objectValue);
        } else {
            std::istringstream iss(tc.arguments);
            if (!Json::parseFromStream(builder, iss, &args, &errors)) {
                // Tool arguments are a JSON object at the public event boundary.
                // Do not forward malformed provider output to clients that parse
                // this field unconditionally.
                args = Json::Value(Json::objectValue);
            } else if (!args.isObject()) {
                Json::Value wrapped(Json::objectValue);
                wrapped["value"] = args;
                args = std::move(wrapped);
            }
        }

        // Apply the safe base contract even when the request did not carry a
        // matching schema. Schema-aware repairs below are optional refinements.
        tc.arguments = toCompactJson(args);

        const Json::Value* toolObj = findToolObj(tc.name);
        if (!toolObj) {
            continue;
        }

        const Json::Value* paramsSchema = nullptr;
        if (toolObj->isMember("function") && (*toolObj)["function"].isObject()) {
            paramsSchema = &(*toolObj)["function"]["parameters"];
        } else {
            paramsSchema = &(*toolObj)["parameters"];
        }
        if (!paramsSchema->isObject() || !paramsSchema->isMember("properties") || !(*paramsSchema)["properties"].isObject()) {
            continue;
        }

        const std::string logicalName =
            tc.originalName.empty() ? tc.name : tc.originalName;

        // 常见别名处理：部分模型返回 ``，而 期望 ``（read_file）。
        if (logicalName == "read_file" && !args.isMember("files") && args.isMember("paths") && args["paths"].isArray()) {
            Json::Value files(Json::arrayValue);
            for (const auto& p : args["paths"]) {
                if (!p.isString()) continue;
                Json::Value obj(Json::objectValue);
                obj["path"] = p.asString();
                files.append(obj);
            }
            if (files.size() > 0) {
                args["files"] = files;
            }
        }

        const auto& props = (*paramsSchema)["properties"];
        for (const auto& paramName : props.getMemberNames()) {
            const auto& paramSchema = props[paramName];
            const std::string type = safeGetType(paramSchema);
            if (type == "array" && paramSchema.isMember("items") &&
                paramSchema["items"].isObject() &&
                safeGetType(paramSchema["items"]) == "object") {
                normalizeArrayOfObjectParam(args, paramName, paramSchema);
            }
        }

        // RooCode 有时会输出不存在的 值（如 ""）用于 ask_followup_question。
        // 将未知 统一归一为空字符串（表示“不切换模式”），确保客户端稳定。
        if (logicalName == "ask_followup_question" && args.isMember("follow_up") && args["follow_up"].isArray()) {
            for (Json::ArrayIndex i = 0; i < args["follow_up"].size(); ++i) {
                auto& item = args["follow_up"][i];
                if (!item.isObject()) continue;

                if (!item.isMember("mode") || !item["mode"].isString()) {
                    item["mode"] = "";
                    continue;
                }

                const std::string mode = toLowerStr(item["mode"].asString());
                if (!(mode.empty() || mode == "code" || mode == "ask" || mode == "architect")) {
                    item["mode"] = "";
                }
            }
        }
        tc.arguments = toCompactJson(args);
    }
}

/**
 * @brief 应用严格客户端规则（仅 Roo/Kilo）
 *
 * 【规则说明】
 * Roo/Kilo 客户端对响应格式有严格要求：
 * - 每次响应必须且只能包含 1 个工具调用
 * - 不允许纯文本响应（必须包装为 attempt_completion）
 * - 不允许多个工具调用（只保留第一个）
 *
 * 【处理逻辑】
 * 1. 如果没有工具调用但有文本 → 包装为 attempt_completion
 * 2. 如果有工具调用 → 清空文本内容（避免重复输出）
 * 3. 如果有多个工具调用 → 只保留第一个
 *
 * 【为什么需要这个规则？】
 * Roo/Kilo 客户端的工作流设计为：
 * - 每轮对话，AI 必须执行一个动作（工具调用）
 * - 客户端根据工具调用类型决定下一步操作
 * - 如果没有工具调用，客户端无法继续工作流
 *
 * @param clientType 客户端类型（用于日志）
 * @param textContent [输入/输出] 文本内容，处理后可能被清空
 * @param toolCalls [输入/输出] 工具调用列表，可能被修改
 */
/**
 * @brief 将不支持原生工具调用的请求转换为“工具桥接模式”
 *
 * 【详细说明】
 * 1. 根据配置选择工具定义输出详细度（compact/full）。
 * 2. 生成严格格式约束提示，并与工具定义拼接。
 * 3. 写入 request.message 供上游模型遵循。
 * 4. 保留 rawMessage/toolsRaw 以便响应阶段解析与兜底。
 */
void toolcall::transformRequestForToolBridge(session_st& session) {
    const std::string clientType = safeJsonAsString(session.provider.clientInfo.get("client_type", ""), "");
    // 请求改写侧同样以能力 IR 为唯一判据，确保与响应侧（emitResultEvents）
    // 对同一请求得出完全一致的结论——两侧若不一致，会出现
    // 「按严格协议下发提示词、却按宽松协议解析响应」的错配。
    const auto clientCaps = actionproto::capabilitiesForClient(
        clientType, session.request.parallelToolCalls);
    const bool strictToolClient = clientCaps.isStrictToolClient();
    // 注意：与响应侧不同，此处不叠加 supportsToolCalls 条件。
    // 请求阶段尚未确定实际通道能力，故对 Codex 一律注入 action 协议提示词；
    // 多注入是安全的（原生支持时模型仍会走结构化 tool_calls），漏注入则会失败。
    const bool codexRooCompat =
        clientCaps.family == actionproto::ClientFamily::Codex;

    Json::Value toolBridgeConfig(Json::objectValue);
    const auto& customConfig = drogon::app().getCustomConfig();
    if (customConfig.isObject() && customConfig.isMember("tool_bridge") &&
        customConfig["tool_bridge"].isObject()) {
        toolBridgeConfig = customConfig["tool_bridge"];
    }
    session.provider.toolBridgeFormat = toolcall::resolveBridgeWireFormat(
        toolBridgeConfig, clientType, session.request.api, session.request.model);
    session.provider.toolBridgeAllowFormatFallback =
        toolcall::resolveBridgeFormatFallback(toolBridgeConfig);
    const auto bridgeCodec = toolcall::createBridgeProtocolCodec(
        session.provider.toolBridgeFormat);
    LOG_INFO << "[生成服务][ToolBridge] 请求格式已固定: format="
             << toolcall::bridgeWireFormatName(session.provider.toolBridgeFormat)
             << ", fallback=" << session.provider.toolBridgeAllowFormatFallback
             << ", client=" << clientType
             << ", channel=" << session.request.api
             << ", model=" << session.request.model;

    // Codex 的上游 system prompt 通常要求 provider-native tool calling。
    // 对文本 bridge 来说该要求不可用；保留它会直接诱发拒绝或纯文本输出。
    // Roo/Kilo 不走此分支，因此不影响其原有 system instructions。
    if (codexRooCompat) {
        const size_t strippedInstructionChars = session.request.systemPrompt.size();
        session.request.systemPrompt.clear();
        LOG_INFO << "[生成服务][CodexRooCompat] 已移除上游可见 Codex instructions: chars="
                 << strippedInstructionChars;
    }

    // 工具定义详细度开关（可配置）
    // 默认规则：compact 表示简化类型，full 表示详细类型。
    bool useFullToolDefinitions = false;
    bool includeToolDescriptions = false; // false=不输出描述；true=输出函数与参数说明
    int maxDescriptionChars = 160;        // 截断描述长度，避免提示词膨胀

    {
        if (toolBridgeConfig.isObject()) {
            const auto& tb = toolBridgeConfig;

            if (tb.isMember("definition_mode") && tb["definition_mode"].isString()) {
                const std::string definitionMode = toLowerStr(tb["definition_mode"].asString());
                if (definitionMode == "full") {
                    useFullToolDefinitions = true;
                } else if (definitionMode == "compact") {
                    useFullToolDefinitions = false;
                } else {
                    LOG_WARN << "[生成服务] tool_bridge.definition_mode 配置无效：" << definitionMode
                             << "，已回退到默认模式（compact）";
                    useFullToolDefinitions = false;
                }
            }

            if (tb.isMember("include_descriptions") && tb["include_descriptions"].isBool()) {
                includeToolDescriptions = tb["include_descriptions"].asBool();
            }

            if (tb.isMember("max_description_chars") && tb["max_description_chars"].isInt()) {
                maxDescriptionChars = tb["max_description_chars"].asInt();
            }

            if (maxDescriptionChars < 0) maxDescriptionChars = 0;
            // 上限由配置自行决定，不再钳制为 2000。
        }
    }

    auto encodeToolList = [&](const Json::Value& tools) -> std::string {
        if (!tools.isArray() || tools.empty()) {
            return "";
        }

        auto join = [](const std::vector<std::string>& items, const std::string& sep) -> std::string {
            std::ostringstream oss;
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) oss << sep;
                oss << items[i];
            }
            return oss.str();
        };

        auto truncate = [&](std::string s) -> std::string {
            s = trimWhitespace(std::move(s));
            if (maxDescriptionChars <= 0) return "";
            if (static_cast<int>(s.size()) <= maxDescriptionChars) return s;
            return s.substr(0, static_cast<size_t>(maxDescriptionChars)) + "...";
        };

        // 从 schema 中收集 required 字段名
        auto getRequiredSet = [](const Json::Value& schema) -> std::unordered_set<std::string> {
            std::unordered_set<std::string> requiredSet;
            if (!schema.isObject()) return requiredSet;

            const auto& required = schema["required"];
            if (required.isArray()) {
                for (const auto& r : required) {
                    if (r.isString()) requiredSet.insert(r.asString());
                }
            }
            return requiredSet;
        };

        // 从 schema 中收集全部属性名（用于嵌套对象）
        auto collectSchemaKeys = [](const Json::Value& schema) -> std::vector<std::string> {
            std::vector<std::string> keys;
            if (!schema.isObject()) return keys;

            const auto& required = schema["required"];
            if (required.isArray()) {
                for (const auto& r : required) {
                    if (r.isString()) keys.push_back(r.asString());
                }
            }

            if (keys.empty() && schema.isMember("properties") && schema["properties"].isObject()) {
                for (const auto& p : schema["properties"].getMemberNames()) {
                    keys.push_back(p);
                }
            }

            return keys;
        };

        // JSON 中的 "" 可能是字符串或数组（例如 ["",""]）。
        // 不要直接调用 asString()，避免触发 JsonCpp 的类型断言错误。
        auto schemaTypeToString = [&](const Json::Value& typeVal) -> std::string {
            if (typeVal.isString()) return typeVal.asString();
            if (typeVal.isArray()) {
                std::vector<std::string> parts;
                parts.reserve(static_cast<size_t>(typeVal.size()));
                for (const auto& t : typeVal) {
                    if (t.isString()) parts.push_back(t.asString());
                }
                if (!parts.empty()) return join(parts, "|");
            }
            return "";
        };

        auto schemaHasType = [&](const Json::Value& schema, const std::string& expected) -> bool {
            if (!schema.isObject() || !schema.isMember("type")) return false;
            const auto& t = schema["type"];
            if (t.isString()) return t.asString() == expected;
            if (t.isArray()) {
                for (const auto& it : t) {
                    if (it.isString() && it.asString() == expected) return true;
                }
            }
            return false;
        };

        auto describeArrayBounds = [](const Json::Value& schema) -> std::string {
            std::string bounds;
            if (schema["minItems"].isIntegral()) {
                bounds += " minItems=" + std::to_string(schema["minItems"].asUInt());
            }
            if (schema["maxItems"].isIntegral()) {
                bounds += " maxItems=" + std::to_string(schema["maxItems"].asUInt());
            }
            return bounds;
        };

        // 保留 JSON Schema enum，避免模型只看到 string 后猜测取值。
        // 为防止工具提示膨胀，最多输出 20 个值，并截断异常长的字符串枚举项。
        auto describeEnumValues = [&](const Json::Value& schema) -> std::string {
            const auto& enumValues = schema["enum"];
            if (!enumValues.isArray() || enumValues.empty()) return "";

            constexpr Json::ArrayIndex kMaxEnumValues = 20;
            constexpr size_t kMaxStringValueChars = 80;
            Json::Value displayed(Json::arrayValue);
            const Json::ArrayIndex count = enumValues.size() < kMaxEnumValues
                ? enumValues.size()
                : kMaxEnumValues;

            for (Json::ArrayIndex i = 0; i < count; ++i) {
                const auto& value = enumValues[i];
                if (value.isString() && value.asString().size() > kMaxStringValueChars) {
                    displayed.append(value.asString().substr(0, kMaxStringValueChars - 3) + "...");
                } else {
                    displayed.append(value);
                }
            }
            if (enumValues.size() > kMaxEnumValues) {
                displayed.append("...");
            }

            return " enum=" + toCompactJson(displayed);
        };

        // compact 也保留数组元素结构、长度约束和枚举取值。
        auto describeSchemaCompact = [&](const Json::Value& schema) -> std::string {
            if (!schema.isObject()) return "any";

            if (schemaHasType(schema, "array")) {
                std::string result = "array";
                const auto& items = schema["items"];
                if (items.isObject()) {
                    if (schemaHasType(items, "object")) {
                        const auto keys = collectSchemaKeys(items);
                        result = "[{" + join(keys, ",") + "}]";
                    } else {
                        const std::string itemType = items.isMember("type")
                            ? schemaTypeToString(items["type"])
                            : "";
                        if (!itemType.empty()) {
                            result = itemType + describeEnumValues(items) + "[]";
                        }
                    }
                }
                return result + describeArrayBounds(schema) + describeEnumValues(schema);
            }

            if (schemaHasType(schema, "object")) {
                const auto keys = collectSchemaKeys(schema);
                return (keys.empty() ? "object" : "{" + join(keys, ",") + "}") +
                    describeEnumValues(schema);
            }

            const std::string typeStr = schema.isMember("type")
                ? schemaTypeToString(schema["type"])
                : "";
            return (typeStr.empty() ? "any" : typeStr) + describeEnumValues(schema);
        };

        // full 模式沿用完整结构表示，参数描述仍由下方单独追加。
        auto describeSchemaFull = [&](const Json::Value& schema) -> std::string {
            return describeSchemaCompact(schema);
        };

        std::ostringstream oss;
        for (const auto& tool : tools) {
            if (!tool.isObject()) continue;
            if (tool.get("type", "").asString() != "function") continue;

            const auto& func = tool["function"];
            if (!func.isObject()) continue;

            const std::string name = func.get("name", "").asString();
            if (name.empty()) continue;

            const std::string funcDesc = func.isMember("description") ? safeJsonAsString(func["description"], "") : "";

            const auto& schema = func["parameters"];
            const auto requiredSet = schema.isObject() ? getRequiredSet(schema) : std::unordered_set<std::string>{};
            const auto& props = schema.isObject() ? schema["properties"] : Json::Value(Json::nullValue);

            if (!useFullToolDefinitions) {
                // 模式：简化类型并使用更清晰格式（不使用 - 或 — 分隔）
                oss << "Tool: " << name << "\n";

                if (includeToolDescriptions) {
                    const std::string t = truncate(funcDesc);
                    if (!t.empty()) {
                        oss << "What: " << t << "\n";
                    }
                }

                oss << "Args:\n";
                if (props.isObject() && !props.getMemberNames().empty()) {
                    for (const auto& key : props.getMemberNames()) {
                        const bool isRequired = requiredSet.find(key) != requiredSet.end();
                        const std::string req = isRequired ? "required" : "optional";

                        std::string typePart = "any";
                        if (props[key].isObject()) {
                            typePart = describeSchemaCompact(props[key]);
                        }

                        oss << "  " << key << " : " << typePart << " [" << req << "]\n";
                    }
                } else {
                    oss << "  (none)\n";
                }

                oss << "\n";
                continue;
            }

            // 模式：详细类型并使用更清晰格式（不使用 - 或 — 分隔）
            oss << "Tool: " << name << "\n";

            if (includeToolDescriptions) {
                const std::string t = truncate(funcDesc);
                if (!t.empty()) {
                    oss << "What: " << t << "\n";
                }
            }

            oss << "Args:\n";
            if (props.isObject() && !props.getMemberNames().empty()) {
                for (const auto& key : props.getMemberNames()) {
                    const bool isRequired = requiredSet.find(key) != requiredSet.end();
                    const std::string req = isRequired ? "required" : "optional";

                    std::string typePart = "any";
                    std::string pDesc;

                    if (props[key].isObject()) {
                        typePart = describeSchemaFull(props[key]);
                        if (includeToolDescriptions && props[key].isMember("description")) {
                            pDesc = truncate(safeJsonAsString(props[key]["description"], ""));
                        }
                    }

                    oss << "  " << key << " : " << typePart << " [" << req << "]";
                    if (includeToolDescriptions && !pDesc.empty()) {
                        oss << " ; " << pDesc;
                    }
                    oss << "\n";
                }
            } else {
                oss << "  (none)\n";
            }

            oss << "\n";
        }

        return oss.str();
    };

    // 为上游编码工具定义。
    // 默认使用 列表，避免描述过长触发上游拒绝
    // 并避免挤占上下文窗口导致用户请求被截断。
    std::string toolDefinitions;
    try {
        toolcall::BridgeDefinitionOptions definitionOptions;
        definitionOptions.includeDescriptions = includeToolDescriptions;
        definitionOptions.maxDescriptionChars = maxDescriptionChars;
        definitionOptions.fullSchema = useFullToolDefinitions;
        toolDefinitions = bridgeCodec->encodeToolDefinitions(
            session.request.tools, definitionOptions);
    } catch (const std::exception& e) {
        // 工具定义编码失败时不得中断请求，回退为仅工具名列表。
        LOG_WARN << "[生成服务] 工具定义编码异常，回退为仅工具名列表: " << e.what();
        std::ostringstream oss;
        for (const auto& tool : session.request.tools) {
            if (!tool.isObject()) continue;
            if (tool.get("type", "").asString() != "function") continue;
            const auto& func = tool["function"];
            if (!func.isObject()) continue;
            const std::string name = func.get("name", "").asString();
            if (!name.empty()) {
                oss << "Tool: " << name << "\n";
            }
        }
        toolDefinitions = oss.str();
    }

    if (toolDefinitions.empty()) {
        LOG_WARN << "[生成服务] 工具定义编码结果为空";
        return;
    }

    // 保留原始输入与工具定义，供下游解析与兜底策略使用。
    if (session.request.rawMessage.empty()) {
        session.request.rawMessage = session.request.message;
    }
    if (session.request.toolsRaw.isNull() || !session.request.toolsRaw.isArray() || session.request.toolsRaw.size() == 0) {
        session.request.toolsRaw = session.request.tools;
    }

    bool rewriteUserInputForBridge = false;
    {
        if (toolBridgeConfig.isObject()) {
            const auto& tb = toolBridgeConfig;
            if (tb.isMember("rewrite_user_input_conflicts") && tb["rewrite_user_input_conflicts"].isBool()) {
                rewriteUserInputForBridge = tb["rewrite_user_input_conflicts"].asBool();
            }
        }
    }

    // 对 bridge 模式下的冲突指令做改写：避免上游继续遵循 native tool-calling 提示。
    rewriteBridgeConflictingDirectives(session, rewriteUserInputForBridge);

    // 解析 tool_choice（同时支持字符串与 JSON 对象两种编码）。
    auto normalizeLower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    std::string toolChoice = "auto";
    std::string forcedToolName;
    if (!session.request.toolChoice.empty()) {
        // JSON 对象形式（例如 {""："",""：{""：""}}）
        if (!session.request.toolChoice.empty() && session.request.toolChoice.front() == '{') {
            Json::Value tc;
            Json::CharReaderBuilder builder;
            std::string errors;
            std::istringstream iss(session.request.toolChoice);
            if (Json::parseFromStream(builder, iss, &tc, &errors) && tc.isObject()) {
                if (tc.get("type", "").asString() == "function" && tc.isMember("function") && tc["function"].isObject()) {
                    const auto& function = tc["function"];
                    forcedToolName = toolcall::makeBridgeToolName(
                        function.get("namespace", tc.get("namespace", "")).asString(),
                        function.get("name", "").asString());
                    if (!forcedToolName.empty()) {
                        toolChoice = "required";
                    }
                }
            }
        } else {
            toolChoice = normalizeLower(session.request.toolChoice);
        }
    }

    int triggerRandomLength = 8;
    {
        if (toolBridgeConfig.isObject()) {
            const auto& tb = toolBridgeConfig;
            if (tb.isMember("trigger_random_length") && tb["trigger_random_length"].isInt()) {
                triggerRandomLength = tb["trigger_random_length"].asInt();
            }
        }
    }

    // 为每次请求生成随机触发标记，仅解析属于本次请求的工具调用，避免误命中
    // 并避免将普通文本中的示例 XML 误解析为真实调用。
    session.provider.toolBridgeTrigger = generateRandomTriggerSignal(static_cast<size_t>(triggerRandomLength));
    const std::string& triggerSignal = session.provider.toolBridgeTrigger;

    const bool hasStrictApplyDiffTool = (strictToolClient || codexRooCompat) &&
        (toolcall::hasToolNamed(session.request.tools, "apply_diff") ||
         toolcall::hasToolNamed(session.request.toolsRaw, "apply_diff"));
    // Glob 静默截断回退：仅在同时存在 Glob 与某个 shell 类工具时注入。
    // 缺少 shell 工具时规则无法执行，注入反而是噪声，因此必须严格前置判断。
    std::string globFallbackShellTool;
    {
        const bool hasGlobTool =
            toolcall::hasToolNamed(session.request.tools, "Glob") ||
            toolcall::hasToolNamed(session.request.toolsRaw, "Glob");
        if (hasGlobTool) {
            static const char* kShellToolCandidates[] = {
                "Shell", "shell", "exec_command", "execute_command",
                "run_command", "Bash", "bash", "terminal"
            };
            for (const char* candidate : kShellToolCandidates) {
                if (toolcall::hasToolNamed(session.request.tools, candidate) ||
                    toolcall::hasToolNamed(session.request.toolsRaw, candidate)) {
                    globFallbackShellTool = candidate;
                    break;
                }
            }
        }
    }
    if (!globFallbackShellTool.empty()) {
        LOG_INFO << "[生成服务][" << clientType
                 << "] 检测到 Glob 与 shell 工具共存，已注入 Glob 截断回退规则, shellTool="
                 << globFallbackShellTool;
    }

    const bool recoveringApplyDiffFailure = hasStrictApplyDiffTool &&
        toolcall::hasApplyDiffFailureContext(
            session.provider.messageContext,
            session.request.message,
            session.request.rawMessage
        );
    if (recoveringApplyDiffFailure) {
        LOG_WARN << "[生成服务][" << clientType
                 << "] 检测到历史 apply_diff 失败，已注入精确匹配与重新读取恢复规则";
    }

    toolcall::BridgePolicyOptions bridgePolicyOptions;
    bridgePolicyOptions.clientType = clientType;
    bridgePolicyOptions.channel = session.request.api;
    bridgePolicyOptions.model = session.request.model;
    bridgePolicyOptions.sentinel = triggerSignal;
    bridgePolicyOptions.toolChoice = toolChoice;
    bridgePolicyOptions.forcedToolName = forcedToolName;
    bridgePolicyOptions.parallelToolCalls = session.request.parallelToolCalls;
    bridgePolicyOptions.requireToolForCurrentRequest =
        codexRooCompat && codexRequestLikelyNeedsExternalState(session.request.message);

    // 历史中的 assistant.tool_calls 与 tool result 必须使用与本次请求相同的 codec。
    bridgeCodec->transformHistory(
        session.provider.messageContext, bridgePolicyOptions);

    // 外层标签与实际动作标签不同，避免定义示例被响应解析器误判。
    {
        std::ostringstream policy;
        policy << "<tool_instructions>\n";
        policy << bridgeCodec->buildPolicy(bridgePolicyOptions);

        if (hasStrictApplyDiffTool) {
            policy << toolcall::buildStrictApplyDiffPolicy(recoveringApplyDiffFailure);
        }

        if (!globFallbackShellTool.empty()) {
            policy << toolcall::buildGlobTruncationFallbackPolicy(globFallbackShellTool);
        }

        policy << "\nAPI Definitions:\n";
        toolDefinitions = policy.str() + toolDefinitions;
        toolDefinitions += "\n</tool_instructions>\n\n";
    }

    LOG_INFO << "[生成服务] 已注入工具定义到请求消息，长度: " << toolDefinitions.length();

    LOG_DEBUG << "[生成服务] 工具定义: " << toolDefinitions;
    static const std::string bridgeNotice =
        "\n\n【注意：回复时必须要满足下面<tool_instructions></tool_instructions>定义中的要求！！！】";

    // Keep a leading environment block separate and explicitly delimit the actual
    // user request. This gives the upstream model stable semantic boundaries:
    // environment context -> user request -> gateway tool instructions.
    const std::string originalMessage = std::move(session.request.message);
    static const std::string environmentCloseTag = "</environment_context>";
    size_t userRequestOffset = 0;
    if (originalMessage.rfind("<environment_context", 0) == 0) {
        const size_t closePos = originalMessage.find(environmentCloseTag);
        if (closePos != std::string::npos) {
            userRequestOffset = closePos + environmentCloseTag.size();
            while (userRequestOffset < originalMessage.size() &&
                   (originalMessage[userRequestOffset] == '\r' ||
                    originalMessage[userRequestOffset] == '\n')) {
                ++userRequestOffset;
            }
        }
    }

    const std::string_view environmentPart(originalMessage.data(), userRequestOffset);
    const std::string_view userRequestPart(
        originalMessage.data() + userRequestOffset,
        originalMessage.size() - userRequestOffset);

    session.request.message.reserve(
        originalMessage.size() + bridgeNotice.size() + toolDefinitions.size() + 40
    );
    session.request.message.append(environmentPart.data(), environmentPart.size());
    if (!environmentPart.empty()) {
        session.request.message.append("\n\n");
    }
    session.request.message.append("<user_request>\n");
    session.request.message.append(userRequestPart.data(), userRequestPart.size());
    session.request.message.append("\n</user_request>");
    session.request.message.append(bridgeNotice);
    session.request.message.append(toolDefinitions);
    session.request.message.append("；");

    // 清空 字段，避免后续流程重复处理
    session.request.tools = Json::Value(Json::nullValue);
    
}
