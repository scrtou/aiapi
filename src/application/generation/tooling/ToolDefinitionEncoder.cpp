#include <application/generation/tooling/ToolDefinitionEncoder.h>
#include <application/generation/tooling/BridgeProtocolCodec.h>
#include <application/generation/tooling/BridgeHelpers.h>
#include <application/generation/tooling/StrictClientRules.h>
#include <application/generation/tooling/ToolDefinitionResolver.h>
#include <application/generation/actionProtocol/ActionProtocolCompiler.h>
#include <platform/Log.h>

#include <cctype>
#include <exception>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>

using namespace bridge;

namespace {

void replaceAll(std::string& text, const std::string& from, const std::string& to)
{
    if (from.empty() || text.empty()) return;
    size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}

void rewriteBridgeConflictsInText(std::string& text)
{
    replaceAll(text, "Use the provider-native tool-calling mechanism.",
               "Use the text bridge tool-calling format defined by <tool_instructions> for this turn.");
    replaceAll(text, "Do not include XML markup or examples.",
               "When a tool call is needed, output ONLY the action format defined by <tool_instructions>.");
    replaceAll(text, "provider-native tool-calling mechanism",
               "text bridge tool-calling format defined by <tool_instructions>");
    replaceAll(text, "<tool_format>native</tool_format>",
               "<tool_format>text_bridge</tool_format>");
    replaceAll(text, "tool_format\":\"native\"", "tool_format\":\"text_bridge\"");
}

void rewriteBridgeConflictsInMessageContext(Json::Value& context, bool rewriteUserMessages)
{
    if (!context.isArray()) return;
    for (auto& message : context) {
        if (!message.isObject() || (!rewriteUserMessages &&
                                    message.get("role", "").asString() == "user")) {
            continue;
        }
        if (message.isMember("content") && message["content"].isString()) {
            std::string content = message["content"].asString();
            rewriteBridgeConflictsInText(content);
            message["content"] = std::move(content);
            continue;
        }
        if (!message.isMember("content") || !message["content"].isArray()) continue;
        for (auto& part : message["content"]) {
            if (!part.isObject() || part.get("type", "").asString() != "text" ||
                !part.isMember("text") || !part["text"].isString()) {
                continue;
            }
            std::string text = part["text"].asString();
            rewriteBridgeConflictsInText(text);
            part["text"] = std::move(text);
        }
    }
}

void rewriteBridgeConflictingDirectives(session_st& session, bool rewriteUserInput)
{
    const size_t systemBefore = session.request.systemPrompt.size();
    const size_t messageBefore = session.request.message.size();
    rewriteBridgeConflictsInText(session.request.systemPrompt);
    if (rewriteUserInput) rewriteBridgeConflictsInText(session.request.message);
    rewriteBridgeConflictsInMessageContext(session.provider.messageContext, rewriteUserInput);
    LOG_INFO << "[生成服务] bridge 冲突指令改写已执行: system_len "
             << systemBefore << "->" << session.request.systemPrompt.size()
             << ", message_len(" << (rewriteUserInput ? "rewritten" : "unchanged_user_input")
             << ") " << messageBefore << "->" << session.request.message.size();
}

bool codexRequestLikelyNeedsExternalState(const std::string& message)
{
    if (message.empty() || message.find("[tool_result") != std::string::npos) return false;
    const std::string lower = toLowerStr(message);
    static const char* keywords[] = {
        "file", "directory", "folder", "repository", "repo", "git ",
        "commit", "branch", "log", "workspace", "codebase", "source code",
        "run ", "execute ", "command", "build", "compile", "test",
        "inspect", "check ", "read ", "open ", "list ", "find ", "search ",
        "文件", "目录", "仓库", "代码", "日志", "提交", "分支", "工作区",
        "运行", "执行", "命令", "构建", "编译", "测试", "检查", "查看",
        "读取", "打开", "列出", "查找", "搜索"
    };
    for (const char* keyword : keywords) {
        if (lower.find(keyword) != std::string::npos) return true;
    }
    return false;
}

toolcall::BridgeDefinitionOptions definitionOptions(const Json::Value& config)
{
    toolcall::BridgeDefinitionOptions options;
    if (!config.isObject()) return options;
    if (config.isMember("definition_mode") && config["definition_mode"].isString()) {
        const std::string mode = toLowerStr(config["definition_mode"].asString());
        if (mode == "full") {
            options.fullSchema = true;
        } else if (mode != "compact") {
            LOG_WARN << "[生成服务] tool_bridge.definition_mode 配置无效：" << mode
                     << "，已回退到默认模式（compact）";
        }
    }
    if (config.isMember("include_descriptions") && config["include_descriptions"].isBool()) {
        options.includeDescriptions = config["include_descriptions"].asBool();
    }
    if (config.isMember("max_description_chars") && config["max_description_chars"].isInt()) {
        options.maxDescriptionChars = config["max_description_chars"].asInt();
    }
    if (options.maxDescriptionChars < 0) options.maxDescriptionChars = 0;
    return options;
}

std::string fallbackToolNames(const Json::Value& tools)
{
    std::ostringstream output;
    for (const auto& tool : tools) {
        if (!tool.isObject() || tool.get("type", "").asString() != "function") continue;
        const auto& function = tool["function"];
        if (!function.isObject()) continue;
        const std::string name = function.get("name", "").asString();
        if (!name.empty()) output << "Tool: " << name << "\n";
    }
    return output.str();
}

std::string encodeDefinitions(const std::shared_ptr<toolcall::IBridgeProtocolCodec>& codec,
                              const Json::Value& tools,
                              const toolcall::BridgeDefinitionOptions& options)
{
    try {
        return codec->encodeToolDefinitions(tools, options);
    } catch (const std::exception& error) {
        LOG_WARN << "[生成服务] 工具定义编码异常，回退为仅工具名列表: " << error.what();
        return fallbackToolNames(tools);
    }
}

bool shouldRewriteUserInput(const Json::Value& config)
{
    return config.isObject() && config.isMember("rewrite_user_input_conflicts") &&
        config["rewrite_user_input_conflicts"].isBool() &&
        config["rewrite_user_input_conflicts"].asBool();
}

struct ToolChoice {
    std::string mode = "auto";
    std::string forcedName;
};

ToolChoice parseToolChoice(const std::string& encoded)
{
    ToolChoice choice;
    if (encoded.empty()) return choice;
    if (encoded.front() != '{') {
        choice.mode = toLowerStr(encoded);
        return choice;
    }

    Json::Value value;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(encoded);
    if (Json::parseFromStream(builder, input, &value, &errors) && value.isObject() &&
        value.get("type", "").asString() == "function" &&
        value.isMember("function") && value["function"].isObject()) {
        const auto& function = value["function"];
        choice.forcedName = toolcall::makeBridgeToolName(
            function.get("namespace", value.get("namespace", "")).asString(),
            function.get("name", "").asString());
        if (!choice.forcedName.empty()) choice.mode = "required";
    }
    return choice;
}

int triggerLength(const Json::Value& config)
{
    if (config.isObject() && config.isMember("trigger_random_length") &&
        config["trigger_random_length"].isInt()) {
        return config["trigger_random_length"].asInt();
    }
    return 8;
}

std::string globFallbackShellTool(const session_st& session)
{
    const bool hasGlob = toolcall::hasToolNamed(session.request.tools, "Glob") ||
        toolcall::hasToolNamed(session.request.toolDefinitionsSource, "Glob");
    if (!hasGlob) return "";
    static const char* candidates[] = {
        "Shell", "shell", "exec_command", "execute_command",
        "run_command", "Bash", "bash", "terminal"
    };
    for (const char* candidate : candidates) {
        if (toolcall::hasToolNamed(session.request.tools, candidate) ||
            toolcall::hasToolNamed(session.request.toolDefinitionsSource, candidate)) {
            return candidate;
        }
    }
    return "";
}

toolcall::BridgePolicyOptions makePolicyOptions(const session_st& session,
                                                 const std::string& clientType,
                                                 const ToolChoice& choice,
                                                 bool codexCompat)
{
    toolcall::BridgePolicyOptions options;
    options.clientType = clientType;
    options.channel = session.request.api;
    options.model = session.request.model;
    options.sentinel = session.provider.toolBridgeTrigger;
    options.toolChoice = choice.mode;
    options.forcedToolName = choice.forcedName;
    options.parallelToolCalls = session.request.parallelToolCalls;
    options.requireToolForCurrentRequest =
        codexCompat && codexRequestLikelyNeedsExternalState(session.request.message);
    return options;
}

std::string decorateDefinitions(const std::shared_ptr<toolcall::IBridgeProtocolCodec>& codec,
                                std::string definitions,
                                const toolcall::BridgePolicyOptions& options,
                                bool hasStrictApplyDiffTool,
                                bool recoveringApplyDiffFailure,
                                const std::string& globFallback)
{
    std::ostringstream policy;
    policy << "<tool_instructions>\n" << codec->buildPolicy(options);
    if (hasStrictApplyDiffTool) {
        policy << toolcall::buildStrictApplyDiffPolicy(recoveringApplyDiffFailure);
    }
    if (!globFallback.empty()) {
        policy << toolcall::buildGlobTruncationFallbackPolicy(globFallback);
    }
    policy << "\nAPI Definitions:\n";
    return policy.str() + definitions + "\n</tool_instructions>\n\n";
}

void wrapUserRequestWithBridgeInstructions(session_st& session,
                                           const std::string& definitions)
{
    static const std::string notice =
        "\n\n【注意：回复时必须要满足下面<tool_instructions></tool_instructions>定义中的要求！！！】";
    static const std::string environmentCloseTag = "</environment_context>";
    const std::string original = std::move(session.request.message);
    size_t userOffset = 0;
    if (original.rfind("<environment_context", 0) == 0) {
        const size_t close = original.find(environmentCloseTag);
        if (close != std::string::npos) {
            userOffset = close + environmentCloseTag.size();
            while (userOffset < original.size() &&
                   (original[userOffset] == '\r' || original[userOffset] == '\n')) {
                ++userOffset;
            }
        }
    }

    const std::string_view environment(original.data(), userOffset);
    const std::string_view userRequest(original.data() + userOffset,
                                       original.size() - userOffset);
    session.request.message.reserve(original.size() + notice.size() + definitions.size() + 40);
    session.request.message.append(environment.data(), environment.size());
    if (!environment.empty()) session.request.message.append("\n\n");
    session.request.message.append("<user_request>\n");
    session.request.message.append(userRequest.data(), userRequest.size());
    session.request.message.append("\n</user_request>");
    session.request.message.append(notice);
    session.request.message.append(definitions);
    session.request.message.append("；");
}

} // namespace

void toolcall::transformRequestForToolBridge(session_st& session,
                                             const Json::Value& runtimeConfig)
{
    const std::string clientType = safeJsonAsString(
        session.provider.clientInfo.get("client_type", ""), "");
    const auto capabilities = actionproto::capabilitiesForClient(
        clientType, session.request.parallelToolCalls);
    const bool strictToolClient = capabilities.isStrictToolClient();
    const bool codexCompat = capabilities.family == actionproto::ClientFamily::Codex;
    const Json::Value config = runtimeConfig.isObject() && runtimeConfig.isMember("tool_bridge") &&
        runtimeConfig["tool_bridge"].isObject()
        ? runtimeConfig["tool_bridge"]
        : Json::Value(Json::objectValue);

    session.provider.toolBridgeFormat = toolcall::resolveBridgeWireFormat(
        config, clientType, session.request.api, session.request.model);
    session.provider.toolBridgeAllowFormatFallback = toolcall::resolveBridgeFormatFallback(config);
    const auto codec = toolcall::createBridgeProtocolCodec(session.provider.toolBridgeFormat);
    LOG_INFO << "[生成服务][ToolBridge] 请求格式已固定: format="
             << toolcall::bridgeWireFormatName(session.provider.toolBridgeFormat)
             << ", fallback=" << session.provider.toolBridgeAllowFormatFallback
             << ", client=" << clientType << ", channel=" << session.request.api
             << ", model=" << session.request.model;

    if (codexCompat) {
        const size_t stripped = session.request.systemPrompt.size();
        session.request.systemPrompt.clear();
        LOG_INFO << "[生成服务][CodexRooCompat] 已移除上游可见 Codex instructions: chars="
                 << stripped;
    }

    std::string definitions = encodeDefinitions(codec, session.request.tools, definitionOptions(config));
    if (definitions.empty()) {
        LOG_WARN << "[生成服务] 工具定义编码结果为空";
        return;
    }
    if (session.request.rawMessage.empty()) session.request.rawMessage = session.request.message;
    if (session.request.toolDefinitionsSource.isNull() || !session.request.toolDefinitionsSource.isArray() ||
        session.request.toolDefinitionsSource.size() == 0) {
        session.request.toolDefinitionsSource = session.request.tools;
    }
    rewriteBridgeConflictingDirectives(session, shouldRewriteUserInput(config));

    const ToolChoice choice = parseToolChoice(session.request.toolChoice);
    session.provider.toolBridgeTrigger = generateRandomTriggerSignal(
        static_cast<size_t>(triggerLength(config)));
    const bool hasStrictApplyDiffTool = (strictToolClient || codexCompat) &&
        (toolcall::hasToolNamed(session.request.tools, "apply_diff") ||
         toolcall::hasToolNamed(session.request.toolDefinitionsSource, "apply_diff"));
    const std::string globFallback = globFallbackShellTool(session);
    if (!globFallback.empty()) {
        LOG_INFO << "[生成服务][" << clientType
                 << "] 检测到 Glob 与 shell 工具共存，已注入 Glob 截断回退规则, shellTool="
                 << globFallback;
    }
    const bool recoveringApplyDiffFailure = hasStrictApplyDiffTool &&
        toolcall::hasApplyDiffFailureContext(session.provider.messageContext,
                                             session.request.message,
                                             session.request.rawMessage);
    if (recoveringApplyDiffFailure) {
        LOG_WARN << "[生成服务][" << clientType
                 << "] 检测到历史 apply_diff 失败，已注入精确匹配与重新读取恢复规则";
    }

    const auto options = makePolicyOptions(session, clientType, choice, codexCompat);
    codec->transformHistory(session.provider.messageContext, options);
    definitions = decorateDefinitions(codec, std::move(definitions), options,
                                     hasStrictApplyDiffTool, recoveringApplyDiffFailure,
                                     globFallback);
    LOG_INFO << "[生成服务] 已注入工具定义到请求消息，长度: " << definitions.length();
    LOG_DEBUG << "[生成服务] 工具定义: " << definitions;
    wrapUserRequestWithBridgeInstructions(session, definitions);
    session.request.tools = Json::Value(Json::nullValue);
}
