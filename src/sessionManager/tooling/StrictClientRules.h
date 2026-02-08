#ifndef STRICT_CLIENT_RULES_H
#define STRICT_CLIENT_RULES_H

#include "sessionManager/contracts/GenerationEvent.h"
#include <string>
#include <vector>

namespace toolcall {

/**
 * @brief 应用严格客户端规则（仅 Roo/Kilo）
 *
 * Roo/Kilo 客户端对响应格式有严格要求：
 * - 每次响应必须且只能包含 1 个工具调用
 * - 不允许纯文本响应（必须包装为 attempt_completion）
 * - 不允许多个工具调用（只保留第一个）
 *
 * @param clientType 客户端类型（用于日志）
 * @param textContent [输入/输出] 文本内容，处理后可能被清空
 * @param toolCalls [输入/输出] 工具调用列表，可能被修改
 */
void applyStrictClientRules(
    const std::string& clientType,
    std::string& textContent,
    std::vector<generation::ToolCallDone>& toolCalls
);

/**
 * @brief 判断客户端是否为严格工具客户端
 *
 * @param clientType 客户端类型字符串
 * @return true 如果是 Kilo-Code 或 RooCode
 */
inline bool isStrictToolClient(const std::string& clientType) {
    return clientType == "Kilo-Code" || clientType == "RooCode";
}

}

#endif
