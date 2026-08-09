#ifndef AIAPI_ACTION_PROTOCOL_ADAPTER_H
#define AIAPI_ACTION_PROTOCOL_ADAPTER_H

#include <sessionManager/actionProtocol/ActionProtocolCompiler.h>
#include <sessionManager/contracts/GenerationEvent.h>
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

// 兼容旧调用点：内部先把 clientType 归一化为能力 IR 再委派。
AdaptedActionResult adaptForClient(const ActionEnvelope& envelope,
                                   const std::string& clientType);

} // namespace actionproto

#endif
