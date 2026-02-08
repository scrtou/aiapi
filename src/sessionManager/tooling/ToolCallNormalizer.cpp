#include "sessionManager/tooling/ToolCallNormalizer.h"
#include <json/json.h>
#include <sstream>

namespace toolcall {

void normalizeToolCallArguments(
    const session_st&,
    std::vector<generation::ToolCallDone>& toolCalls
) {
    Json::CharReaderBuilder reader;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";

    for (auto& toolCall : toolCalls) {
        Json::Value args;
        std::string parseErrors;

        if (toolCall.arguments.empty()) {
            toolCall.arguments = "{}";
            continue;
        }

        std::istringstream input(toolCall.arguments);
        if (!Json::parseFromStream(reader, input, &args, &parseErrors)) {
            toolCall.arguments = "{}";
            continue;
        }

        if (!args.isObject()) {
            Json::Value wrapped(Json::objectValue);
            wrapped["value"] = args;
            toolCall.arguments = Json::writeString(writer, wrapped);
            continue;
        }

        toolCall.arguments = Json::writeString(writer, args);
    }
}

}
