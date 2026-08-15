#include <application/generation/tooling/ToolCallNormalizer.h>
#include <application/generation/tooling/BridgeHelpers.h>
#include <application/generation/tooling/ToolDefinitionResolver.h>
#include <platform/Log.h>

#include <sstream>
#include <string>
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

std::string schemaType(const Json::Value& schema)
{
    if (!schema.isObject() || !schema.isMember("type")) return "";
    const auto& type = schema["type"];
    if (type.isString()) return type.asString();
    if (!type.isArray()) return "";
    for (const auto& candidate : type) {
        if (candidate.isString() && candidate.asString() != "null") {
            return candidate.asString();
        }
    }
    return type.empty() || !type[0].isString() ? "" : type[0].asString();
}

Json::Value parseObjectArguments(const std::string& encoded)
{
    if (encoded.empty()) return Json::Value(Json::objectValue);

    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(encoded);
    if (!Json::parseFromStream(builder, input, &parsed, &errors)) {
        return Json::Value(Json::objectValue);
    }
    if (parsed.isObject()) return parsed;

    Json::Value wrapped(Json::objectValue);
    wrapped["value"] = std::move(parsed);
    return wrapped;
}

std::vector<std::string> requiredKeys(const Json::Value& schema)
{
    std::vector<std::string> keys;
    const auto& required = schema["required"];
    if (!required.isArray()) return keys;
    for (const auto& key : required) {
        if (key.isString()) keys.push_back(key.asString());
    }
    return keys;
}

std::string arrayElementKey(const std::string& parameter,
                            const std::vector<std::string>& keys)
{
    if (parameter == "files") return "path";
    for (const char* preferred : {"text", "path", "name", "id"}) {
        for (const auto& key : keys) {
            if (key == preferred) return key;
        }
    }
    return keys.front();
}

std::string propertyType(const Json::Value* properties, const std::string& key)
{
    if (!properties || !properties->isObject() || !properties->isMember(key) ||
        !(*properties)[key].isObject()) {
        return "";
    }
    return schemaType((*properties)[key]);
}

void normalizeArrayOfObjects(Json::Value& arguments,
                             const std::string& parameter,
                             const Json::Value& schema)
{
    if (!arguments.isObject() || !arguments.isMember(parameter) ||
        !arguments[parameter].isArray() || schemaType(schema) != "array") {
        return;
    }
    const auto& items = schema["items"];
    if (!items.isObject() || schemaType(items) != "object") return;

    const std::vector<std::string> keys = requiredKeys(items);
    if (keys.empty()) return;
    const Json::Value* properties =
        items.isMember("properties") && items["properties"].isObject()
            ? &items["properties"]
            : nullptr;

    const Json::ArrayIndex originalSize = arguments[parameter].size();
    Json::Value normalized(Json::arrayValue);
    for (const auto& element : arguments[parameter]) {
        Json::Value object;
        if (element.isString()) {
            object = Json::Value(Json::objectValue);
            object[arrayElementKey(parameter, keys)] = element.asString();
        } else if (element.isObject()) {
            object = element;
        } else {
            continue;
        }

        for (const auto& key : keys) {
            if (!object.isMember(key) && key == "mode") object[key] = "";
        }
        if (parameter == "files" && !object.isMember("path")) {
            if (object.isMember("file") && object["file"].isString()) {
                object["path"] = object["file"].asString();
            } else if (object.isMember("name") && object["name"].isString()) {
                object["path"] = object["name"].asString();
            }
        }

        bool valid = true;
        for (const auto& key : keys) {
            if (key == "mode") {
                if (!object.isMember(key) || !object[key].isString()) object[key] = "";
                continue;
            }
            if (!object.isMember(key) || object[key].isNull()) {
                valid = false;
                break;
            }
            if (propertyType(properties, key) == "string" && !object[key].isString()) {
                object[key] = toCompactJson(object[key]);
            }
        }
        if (valid && parameter == "files" &&
            (!object.isMember("path") || !object["path"].isString() ||
             object["path"].asString().empty())) {
            valid = false;
        }
        if (valid) normalized.append(std::move(object));
    }

    if (normalized.size() != originalSize) {
        LOG_WARN << "[生成服务] 数组参数规范化未完成，保留原始参数: "
                 << parameter << ", original=" << originalSize
                 << ", normalized=" << normalized.size();
        return;
    }
    if (schema["minItems"].isIntegral() &&
        normalized.size() < schema["minItems"].asUInt()) {
        LOG_WARN << "[生成服务] 数组参数规范化结果不满足 minItems，保留原始参数: "
                 << parameter;
        return;
    }
    arguments[parameter] = std::move(normalized);
}

void normalizeReadFileAlias(Json::Value& arguments, const std::string& logicalName)
{
    if (logicalName != "read_file" || arguments.isMember("files") ||
        !arguments.isMember("paths") || !arguments["paths"].isArray()) {
        return;
    }
    Json::Value files(Json::arrayValue);
    for (const auto& path : arguments["paths"]) {
        if (!path.isString()) continue;
        Json::Value file(Json::objectValue);
        file["path"] = path.asString();
        files.append(std::move(file));
    }
    if (files.size() > 0) arguments["files"] = std::move(files);
}

void normalizeFollowUpModes(Json::Value& arguments, const std::string& logicalName)
{
    if (logicalName != "ask_followup_question" || !arguments.isMember("follow_up") ||
        !arguments["follow_up"].isArray()) {
        return;
    }
    for (Json::ArrayIndex index = 0; index < arguments["follow_up"].size(); ++index) {
        auto& item = arguments["follow_up"][index];
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

const Json::Value* parameterSchema(const toolcall::ToolDefinitionMatch& definition)
{
    if (!definition.callable || !definition.callable->isObject()) return nullptr;
    const auto& schema = (*definition.callable)["parameters"];
    if (!schema.isObject() || !schema.isMember("properties") ||
        !schema["properties"].isObject()) {
        return nullptr;
    }
    return &schema;
}

} // namespace

void toolcall::normalizeToolCallArguments(
    const session_st& session,
    std::vector<generation::ToolCallDone>& toolCalls)
{
    if (toolCalls.empty()) return;

    const Json::Value& definitions =
        (!session.request.toolsRaw.isNull() && session.request.toolsRaw.isArray() &&
         session.request.toolsRaw.size() > 0)
            ? session.request.toolsRaw
            : session.request.tools;

    for (auto& call : toolCalls) {
        Json::Value arguments = parseObjectArguments(call.arguments);
        call.arguments = toCompactJson(arguments);

        const auto definition = toolcall::findToolDefinition(definitions, call.name);
        if (!definition.has_value()) continue;
        const Json::Value* schema = parameterSchema(*definition);
        if (!schema) continue;

        const std::string logicalName = call.originalName.empty()
            ? call.name
            : call.originalName;
        normalizeReadFileAlias(arguments, logicalName);
        for (const auto& parameter : (*schema)["properties"].getMemberNames()) {
            const auto& property = (*schema)["properties"][parameter];
            if (schemaType(property) == "array" && property.isMember("items") &&
                property["items"].isObject() && schemaType(property["items"]) == "object") {
                normalizeArrayOfObjects(arguments, parameter, property);
            }
        }
        normalizeFollowUpModes(arguments, logicalName);
        call.arguments = toCompactJson(arguments);
    }
}
