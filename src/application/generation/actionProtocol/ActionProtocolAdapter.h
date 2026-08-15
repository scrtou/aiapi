#ifndef AIAPI_ACTION_PROTOCOL_ADAPTER_H
#define AIAPI_ACTION_PROTOCOL_ADAPTER_H

#include <application/generation/actionProtocol/ActionProtocolCompiler.h>
#include <application/generation/contracts/GenerationEvent.h>
#include <string>
#include <vector>

namespace actionproto {

struct AdaptedActionResult {
    std::vector<generation::ToolCallDone> toolCalls;
    std::string text;
};

// 将客户端无关的 ActionEnvelope 转换为内部 GenerationEvent 语义。
// 适配器只读取能力 IR：收尾工具名由 capabilities.completionToolName 决定，
// 客户端字符串不再出现在本层。
AdaptedActionResult adaptForCapabilities(const ActionEnvelope& envelope,
                                         const ClientCapabilities& capabilities);

} // namespace actionproto

#endif
