#ifndef TOOL_DEFINITION_ENCODER_H
#define TOOL_DEFINITION_ENCODER_H

#include "sessionManager/core/Session.h"
#include <string>

namespace toolcall {

/**
 * @brief 为 ToolCallBridge 转换请求
 *
 * 当通道不支持原生 tool calls 时，将工具定义注入到
 * session.request.message 和 session.request.systemPrompt，
 * 生成随机触发标记（Toolify-style），并清除 session.request.tools。
 *
 * @param session 会话状态（会被修改）
 */
void transformRequestForToolBridge(session_st& session);

}

#endif
