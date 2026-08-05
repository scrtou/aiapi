#ifndef AIAPI_ACTION_PROTOCOL_ADAPTER_H
#define AIAPI_ACTION_PROTOCOL_ADAPTER_H

#include "sessionManager/actionProtocol/ActionProtocolCompiler.h"
#include "sessionManager/contracts/GenerationEvent.h"
#include <string>
#include <vector>

namespace actionproto {

struct AdaptedActionResult {
    std::vector<generation::ToolCallDone> toolCalls;
    std::string text;
};

// 将客户端无关的 ActionEnvelope 转换为内部 GenerationEvent 语义。
// 客户端专用名称（例如 Roo 的 attempt_completion）只允许出现在适配器中。
AdaptedActionResult adaptForClient(const ActionEnvelope& envelope,
                                   const std::string& clientType);

} // namespace actionproto

#endif
