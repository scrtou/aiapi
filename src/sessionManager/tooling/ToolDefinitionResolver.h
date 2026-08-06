#ifndef TOOL_DEFINITION_RESOLVER_H
#define TOOL_DEFINITION_RESOLVER_H

#include <json/json.h>
#include <optional>
#include <string>
#include <utility>

namespace toolcall {

inline std::string appendNamespaceSegment(const std::string& parent,
                                          const std::string& child)
{
    if (parent.empty()) return child;
    if (child.empty()) return parent;
    return parent + "__" + child;
}

inline std::string makeBridgeToolName(const std::string& namespacePath,
                                      const std::string& originalName)
{
    return appendNamespaceSegment(namespacePath, originalName);
}

struct ToolDefinitionMatch {
    const Json::Value* tool = nullptr;
    const Json::Value* callable = nullptr;
    std::string type;
    std::string originalName;
    std::string namespacePath;
    std::string bridgeName;

    explicit operator bool() const
    {
        return tool != nullptr && callable != nullptr && !bridgeName.empty();
    }
};

inline std::string toolDefinitionName(const Json::Value& tool)
{
    if (!tool.isObject()) return "";
    if (tool.isMember("function") && tool["function"].isObject()) {
        return tool["function"].get("name", "").asString();
    }
    return tool.get("name", "").asString();
}

namespace detail {

template <typename Visitor>
bool visitToolDefinitionsImpl(const Json::Value& tools,
                              const std::string& parentNamespace,
                              Visitor& visitor)
{
    if (!tools.isArray()) return true;

    for (const auto& tool : tools) {
        if (!tool.isObject()) continue;

        const std::string declaredType = tool.get("type", "").asString();
        if (declaredType == "namespace") {
            const std::string namespaceName = tool.get("name", "").asString();
            if (namespaceName.empty() || !tool["tools"].isArray()) continue;

            const std::string childNamespace =
                appendNamespaceSegment(parentNamespace, namespaceName);
            if (!visitToolDefinitionsImpl(tool["tools"], childNamespace, visitor)) {
                return false;
            }
            continue;
        }

        if (declaredType != "function" && declaredType != "custom") continue;

        const Json::Value* callable = &tool;
        if (tool.isMember("function") && tool["function"].isObject()) {
            callable = &tool["function"];
        }

        const std::string declaredName = callable->get("name", "").asString();
        const std::string originalName =
            callable->get("_aiapi_original_name", declaredName).asString();
        if (originalName.empty()) continue;

        const std::string metadataNamespace =
            callable->get("_aiapi_namespace", "").asString();
        const std::string namespacePath = metadataNamespace.empty()
            ? parentNamespace
            : metadataNamespace;

        std::string bridgeName;
        if (!metadataNamespace.empty() && !declaredName.empty()) {
            bridgeName = declaredName;
        } else {
            bridgeName = makeBridgeToolName(namespacePath, originalName);
        }

        std::string originalType =
            callable->get("_aiapi_original_type", "").asString();
        if (originalType == "namespace_function") originalType = "function";
        if (originalType != "custom") {
            originalType = declaredType == "custom" ? "custom" : "function";
        }

        ToolDefinitionMatch match;
        match.tool = &tool;
        match.callable = callable;
        match.type = std::move(originalType);
        match.originalName = originalName;
        match.namespacePath = namespacePath;
        match.bridgeName = std::move(bridgeName);
        if (!visitor(match)) return false;
    }

    return true;
}

} // namespace detail

template <typename Visitor>
bool visitToolDefinitions(const Json::Value& tools, Visitor visitor)
{
    return detail::visitToolDefinitionsImpl(tools, "", visitor);
}

inline std::optional<ToolDefinitionMatch> findToolDefinition(
    const Json::Value& tools,
    const std::string& bridgeName)
{
    std::optional<ToolDefinitionMatch> found;
    visitToolDefinitions(tools, [&](const ToolDefinitionMatch& match) {
        if (match.bridgeName != bridgeName) return true;
        found = match;
        return false;
    });
    return found;
}

inline std::optional<ToolDefinitionMatch> findToolDefinition(
    const Json::Value& tools,
    const std::string& namespacePath,
    const std::string& originalName)
{
    std::optional<ToolDefinitionMatch> found;
    visitToolDefinitions(tools, [&](const ToolDefinitionMatch& match) {
        if (match.namespacePath != namespacePath ||
            match.originalName != originalName) {
            return true;
        }
        found = match;
        return false;
    });
    return found;
}

inline std::optional<ToolDefinitionMatch> firstToolDefinition(
    const Json::Value& tools)
{
    std::optional<ToolDefinitionMatch> found;
    visitToolDefinitions(tools, [&](const ToolDefinitionMatch& match) {
        found = match;
        return false;
    });
    return found;
}

inline const Json::Value* toolParametersSchema(const ToolDefinitionMatch& match)
{
    if (!match.callable || !match.callable->isObject()) return nullptr;
    const auto& parameters = (*match.callable)["parameters"];
    return parameters.isObject() ? &parameters : nullptr;
}

} // namespace toolcall

#endif
