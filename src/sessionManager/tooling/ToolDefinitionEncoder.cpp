#include "sessionManager/tooling/ToolDefinitionEncoder.h"
#include "sessionManager/tooling/BridgeHelpers.h"
#include "sessionManager/tooling/XmlTagToolCallCodec.h"
namespace toolcall {

void transformRequestForToolBridge(session_st& session) {
    if (!session.request.tools.isArray() || session.request.tools.empty()) {
        return;
    }

    auto codec = createXmlTagToolCallCodec();
    const std::string encodedTools = codec->encodeToolDefinitions(session.request.tools);

    if (!session.request.systemPrompt.empty()) {
        session.request.systemPrompt += "\n\n";
    }
    session.request.systemPrompt += encodedTools;

    session.provider.toolBridgeTrigger = bridge::generateRandomTriggerSignal();
    if (session.request.message.empty()) {
        session.request.message = session.provider.toolBridgeTrigger;
    } else {
        session.request.message = session.provider.toolBridgeTrigger + "\n" + session.request.message;
    }

    session.request.tools = Json::Value(Json::arrayValue);
}

}
