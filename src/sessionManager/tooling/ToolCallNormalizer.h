#ifndef TOOL_CALL_NORMALIZER_H
#define TOOL_CALL_NORMALIZER_H

#include "sessionManager/core/Session.h"
#include "sessionManager/contracts/GenerationEvent.h"
#include <string>
#include <vector>

namespace toolcall {

/**
 * @brief 规范化 tool call 参数形状
 *
 * 根据客户端提供的 JSONSchema 规范化参数，修复常见问题：
 * - 数组元素类型错误（string[] → object[]）
 * - 参数别名（paths → files）
 * - 缺失可选字段默认值（mode → ""）
 * - ask_followup_question 的 mode 规范化
 *
 * @param session 会话状态（包含工具定义）
 * @param toolCalls tool calls 列表（会被修改）
 */
void normalizeToolCallArguments(
    const session_st& session,
    std::vector<generation::ToolCallDone>& toolCalls
);

}

#endif
