#ifndef STRICT_CLIENT_RULES_H
#define STRICT_CLIENT_RULES_H

#include <application/generation/actionProtocol/ActionProtocolCompiler.h>
#include <application/generation/contracts/GenerationEvent.h>
#include <json/json.h>
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
 * 判定完全委派给客户端能力 IR，本层不再比较 clientType 字符串，
 * 因此 RooCode / Roo-Code / roocode 等变体自动等价。
 *
 * 本函数是全项目唯一的 isStrictToolClient(clientType) 定义（inline，
 * 曾在 ToolCallValidator.cpp 中存在重复实现，已删除）；新增调用点直接
 * 包含本头文件即可，切勿再就地复制一份。
 *
 * 此处固定 parallelToolCalls=false：严格性只由
 * requiresActionEveryTurn / completionToolName 决定，与并行开关无关，
 * 传入 false 可让调用方无需持有请求上下文。
 */
inline bool isStrictToolClient(const std::string& clientType) {
    return actionproto::capabilitiesForClient(clientType,
                                             /*parallelToolCalls=*/false)
        .isStrictToolClient();
}

/**
 * @brief 判断工具定义列表中是否存在指定函数工具
 */
bool hasToolNamed(const Json::Value& tools, const std::string& toolName);

/**
 * @brief 检测最近一次 apply_diff 失败是否尚未被后续 read_file 结果消解
 */
bool hasApplyDiffFailureContext(
    const Json::Value& messageContext,
    const std::string& currentMessage,
    const std::string& rawMessage
);

/**
 * @brief 构建 Roo/Kilo 的 apply_diff 精确匹配与失败恢复规则
 */
std::string buildStrictApplyDiffPolicy(bool recoveringFromFailure);

// 构建 Glob 静默截断回退规则。
// 仅在同时提供了 Glob 与某个 shell 类工具时才有意义，调用方需自行判断。
// shellToolName 为实际可用的 shell 工具名（如 Shell / exec_command）。
std::string buildGlobTruncationFallbackPolicy(const std::string& shellToolName);

}

#endif
