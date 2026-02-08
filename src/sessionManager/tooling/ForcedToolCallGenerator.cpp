#include "sessionManager/tooling/ForcedToolCallGenerator.h"
#include "sessionManager/tooling/BridgeHelpers.h"
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

    std::string toolName;
    if (session.request.tools.isArray() && !session.request.tools.empty()) {
        const auto& first = session.request.tools[0U];
        if (first.isObject() && first.isMember("function") && first["function"].isObject()) {
            toolName = first["function"].get("name", "").asString();
        }
        if (toolName.empty() && first.isMember("name")) {
            toolName = first["name"].asString();
        }
    }

    if (toolName.empty()) {
        toolName = "attempt_completion";
    }

    generation::ToolCallDone fallback;
    fallback.id = bridge::generateFallbackToolCallId();
    fallback.name = toolName;
    fallback.index = 0;

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
