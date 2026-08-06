#include "sessionManager/tooling/ForcedToolCallGenerator.h"
#include "sessionManager/tooling/BridgeHelpers.h"
#include "sessionManager/tooling/ToolDefinitionResolver.h"
#include <json/json.h>

namespace toolcall {

void generateForcedToolCall(
    const session_st& session,
    std::vector<generation::ToolCallDone>& outToolCalls,
    std::string& outTextContent
) {
    if (!outToolCalls.empty()) {
        return;
    }

    auto selected = firstToolDefinition(session.request.tools);
    if (!selected.has_value()) {
        selected = firstToolDefinition(session.request.toolsRaw);
    }

    const std::string toolName = selected.has_value()
        ? selected->bridgeName
        : "attempt_completion";

    generation::ToolCallDone fallback;
    fallback.id = bridge::generateFallbackToolCallId();
    fallback.name = toolName;
    fallback.index = 0;
    if (selected.has_value()) {
        fallback.originalName = selected->originalName;
        fallback.namespacePath = selected->namespacePath;
        fallback.type = selected->type;
    }

    Json::Value args(Json::objectValue);
    if (toolName == "attempt_completion") {
        args["result"] = outTextContent.empty() ? session.request.message : outTextContent;
    } else {
        args["query"] = session.request.message;
    }

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    fallback.arguments = Json::writeString(writer, args);

    outToolCalls.push_back(std::move(fallback));
    outTextContent.clear();
}

}
