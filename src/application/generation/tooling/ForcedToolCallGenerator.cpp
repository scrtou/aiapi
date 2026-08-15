#include <application/generation/tooling/ForcedToolCallGenerator.h>
#include <application/generation/tooling/BridgeHelpers.h>
#include <application/generation/tooling/ToolDefinitionResolver.h>
#include <platform/Log.h>

#include <algorithm>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

using namespace bridge;

namespace {

Json::StreamWriterBuilder& compactJsonWriter()
{
    static thread_local Json::StreamWriterBuilder writer = [] {
        Json::StreamWriterBuilder instance;
        instance["indentation"] = "";
        return instance;
    }();
    return writer;
}

std::string toCompactJson(const Json::Value& value)
{
    return Json::writeString(compactJsonWriter(), value);
}

struct ForcedChoice {
    bool required = false;
    std::string toolName;
};

ForcedChoice parseForcedChoice(const std::string& encodedChoice)
{
    ForcedChoice choice;
    if (encodedChoice.empty()) return choice;

    if (encodedChoice.front() != '{') {
        choice.required = toLowerStr(encodedChoice) == "required";
        return choice;
    }

    Json::Value value;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(encodedChoice);
    if (!Json::parseFromStream(builder, input, &value, &errors) ||
        !value.isObject() || value.get("type", "").asString() != "function" ||
        !value.isMember("function") || !value["function"].isObject()) {
        return choice;
    }

    const auto& function = value["function"];
    choice.toolName = toolcall::makeBridgeToolName(
        function.get("namespace", value.get("namespace", "")).asString(),
        function.get("name", "").asString());
    choice.required = !choice.toolName.empty();
    return choice;
}

bool endsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string stripTrailingPunctuation(std::string value)
{
    value = trimWhitespace(std::move(value));
    static const std::vector<std::string> suffixes = {
        "？", "?", "！", "!", "。", ".", "，", ",", "；", ";", ":", "："
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& suffix : suffixes) {
            if (!endsWith(value, suffix)) continue;
            value.erase(value.size() - suffix.size());
            value = trimWhitespace(std::move(value));
            changed = true;
        }
    }
    return value;
}

std::string lastToken(std::string value)
{
    value = trimWhitespace(std::move(value));
    const size_t pos = value.find_last_of(" \t\r\n");
    return pos == std::string::npos ? value : trimWhitespace(value.substr(pos + 1));
}

std::string extractLocationLike(std::string input)
{
    std::string text = stripTrailingPunctuation(trimWhitespace(std::move(input)));
    size_t marker = text.find("天气");
    if (marker == std::string::npos) marker = text.find("气温");
    if (marker != std::string::npos) {
        std::string before = stripTrailingPunctuation(text.substr(0, marker));
        static const std::vector<std::string> prefixes = {
            "请帮我查一下", "请帮我查下", "请帮我查查", "请帮我查", "帮我查一下", "帮我查下", "帮我查查", "帮我查",
            "麻烦查一下", "麻烦查下", "麻烦查查", "麻烦查", "查一下", "查下", "查查", "查询", "看看", "看一下", "请问", "问一下"
        };
        bool removed = true;
        while (removed) {
            removed = false;
            for (const auto& prefix : prefixes) {
                if (before.rfind(prefix, 0) != 0) continue;
                before = trimWhitespace(before.substr(prefix.size()));
                removed = true;
                break;
            }
        }
        static const std::vector<std::string> timeSuffixes = {
            "今天", "明天", "后天", "现在", "当前", "目前"
        };
        before = stripTrailingPunctuation(trimWhitespace(std::move(before)));
        for (const auto& suffix : timeSuffixes) {
            if (endsWith(before, suffix)) {
                before.erase(before.size() - suffix.size());
                before = trimWhitespace(std::move(before));
            }
        }
        if (endsWith(before, "的")) {
            before.erase(before.size() - std::string("的").size());
            before = trimWhitespace(std::move(before));
        }
        before = lastToken(std::move(before));
        if (!before.empty()) return before;
    }

    const std::string lower = toLowerStr(text);
    constexpr std::string_view kWeatherIn = "weather in ";
    const size_t english = lower.find(kWeatherIn);
    if (english != std::string::npos) {
        std::string location = stripTrailingPunctuation(
            trimWhitespace(text.substr(english + kWeatherIn.size())));
        if (!location.empty()) return location;
    }
    return stripTrailingPunctuation(trimWhitespace(std::move(text)));
}

bool isLocationParameter(const std::string& name)
{
    const std::string lower = toLowerStr(name);
    return lower.find("location") != std::string::npos ||
        lower.find("city") != std::string::npos ||
        lower.find("place") != std::string::npos ||
        lower.find("address") != std::string::npos ||
        lower.find("region") != std::string::npos;
}

Json::Value makeForcedArguments(const toolcall::ToolDefinitionMatch& definition,
                                const std::string& sourceText)
{
    Json::Value arguments(Json::objectValue);
    const Json::Value* schema = toolcall::toolParametersSchema(definition);
    if (!schema) return arguments;

    std::vector<std::string> parameters;
    const auto& required = (*schema)["required"];
    if (required.isArray()) {
        for (const auto& item : required) {
            if (item.isString()) parameters.push_back(item.asString());
        }
    }
    if (parameters.empty() && schema->isMember("properties") &&
        (*schema)["properties"].isObject()) {
        parameters = (*schema)["properties"].getMemberNames();
    }

    for (const auto& parameter : parameters) {
        if (parameter.empty()) continue;
        arguments[parameter] = isLocationParameter(parameter)
            ? extractLocationLike(sourceText)
            : trimWhitespace(sourceText);
    }
    return arguments;
}

} // namespace

void toolcall::generateForcedToolCall(
    const session_st& session,
    std::vector<generation::ToolCallDone>& outToolCalls,
    std::string& outTextContent)
{
    const ForcedChoice choice = parseForcedChoice(session.request.toolChoice);
    const Json::Value& toolDefinitions =
        (!session.request.toolDefinitionsSource.isNull() && session.request.toolDefinitionsSource.isArray() &&
         session.request.toolDefinitionsSource.size() > 0)
            ? session.request.toolDefinitionsSource
            : session.request.tools;
    if (!choice.required || !toolDefinitions.isArray() || toolDefinitions.empty()) {
        return;
    }

    auto definition = choice.toolName.empty()
        ? toolcall::firstToolDefinition(toolDefinitions)
        : toolcall::findToolDefinition(toolDefinitions, choice.toolName);
    if (!definition.has_value()) {
        definition = toolcall::firstToolDefinition(toolDefinitions);
    }
    if (!definition.has_value()) return;

    const std::string sourceText = session.request.rawMessage.empty()
        ? session.request.message
        : session.request.rawMessage;
    const Json::Value arguments = makeForcedArguments(*definition, sourceText);

    generation::ToolCallDone call;
    call.id = generateFallbackToolCallId();
    call.name = definition->bridgeName;
    call.originalName = definition->originalName;
    call.namespacePath = definition->namespacePath;
    call.type = definition->type;
    call.index = 0;
    call.arguments = toCompactJson(arguments);
    outToolCalls.push_back(call);
    outTextContent.clear();

    LOG_WARN << "[生成服务] 上游未返回工具调用，已根据 tool_choice=required 生成兜底工具调用："
             << call.name;
    Json::Value detail;
    detail["tool_name"] = call.name;
    detail["tool_choice"] = session.request.toolChoice;
    detail["generated_args"] = arguments;
    recordWarnStat(
        session, metrics::Domain::TOOL_BRIDGE,
        metrics::EventType::TOOLBRIDGE_FORCED_TOOLCALL_GENERATED,
        "已生成强制工具调用: " + call.name, detail,
        sourceText.substr(0, std::min(sourceText.size(), size_t(512))), call.name);
}
