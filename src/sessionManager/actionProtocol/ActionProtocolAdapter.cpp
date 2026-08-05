#include "sessionManager/actionProtocol/ActionProtocolAdapter.h"

#include <json/json.h>
#include <sstream>

namespace actionproto {

AdaptedActionResult adaptForClient(const ActionEnvelope& envelope,
                                   const std::string& clientType) {
    AdaptedActionResult result;
    result.text = envelope.finalResponse;
    result.toolCalls.reserve(envelope.toolCalls.size());
    for (size_t i = 0; i < envelope.toolCalls.size(); ++i) {
        generation::ToolCallDone call;
        call.id = envelope.toolCalls[i].id;
        call.name = envelope.toolCalls[i].name;
        call.arguments = envelope.toolCalls[i].argumentsJson;
        call.index = static_cast<int>(i);
        call.type = "function";
        result.toolCalls.push_back(std::move(call));
    }

    // Roo/Kilo 的历史输出协议要求最终回答是一个虚拟工具调用；其余客户端
    // 直接使用统一的文本语义。该差异不能泄漏到 ActionProtocolCompiler。
    if (result.toolCalls.empty() &&
        (clientType == "RooCode" || clientType == "Kilo-Code")) {
        generation::ToolCallDone completion;
        completion.id = "action_completion_0";
        completion.name = "attempt_completion";
        completion.index = 0;
        Json::Value args(Json::objectValue);
        args["result"] = result.text;
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        completion.arguments = Json::writeString(writer, args);
        result.toolCalls.push_back(std::move(completion));
        result.text.clear();
    }
    return result;
}

} // namespace actionproto
