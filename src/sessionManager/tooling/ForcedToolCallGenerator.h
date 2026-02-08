#ifndef FORCED_TOOL_CALL_GENERATOR_H
#define FORCED_TOOL_CALL_GENERATOR_H

#include "sessionManager/core/Session.h"
#include "sessionManager/contracts/GenerationEvent.h"
#include <string>
#include <vector>

namespace toolcall {

/**
 * @brief 生成 tool_choice=required 时的兜底 tool call
 *
 * 当上游模型未返回任何 tool call，但 tool_choice=required 时，
 * 根据工具定义和用户输入自动生成一个工具调用。
 *
 * @param session 会话状态
 * @param outToolCalls 输出的 tool calls 列表
 * @param outTextContent 输出的文本内容（会被清空）
 */
void generateForcedToolCall(
    const session_st& session,
    std::vector<generation::ToolCallDone>& outToolCalls,
    std::string& outTextContent
);

}

#endif
