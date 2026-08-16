#include <application/generation/actionProtocol/ActionProtocolAdapter.h>

#include <json/json.h>
#include <sstream>

namespace actionproto {

// 适配层：把与客户端无关的 ActionEnvelope 投影为特定客户端可消费的
// (文本 + 工具调用) 组合。所有差异化处理都只读 ClientCapabilities，
// 因此本文件不含任何 clientType 字符串比较。
//
// 处理顺序是有意安排的，调整会改变语义：
//   1) 原样搬运工具调用并编号
//   2) 按 maxToolCalls 截断        —— 必须在补收尾工具之前
//   3) 空调用时补虚拟收尾工具      —— 依赖第 2 步后的真实空状态
//   4) 按需清空伴随文本            —— 需要看到第 3 步补出的调用
AdaptedActionResult adaptForCapabilities(const ActionEnvelope& envelope,
                                         const ClientCapabilities& capabilities) {
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

    // 只保留能力 IR 允许的调用数量（Roo/Kilo 恒为 1）。
    if (capabilities.coalescesParallelCalls &&
        result.toolCalls.size() > capabilities.maxToolCalls) {
        result.toolCalls.resize(capabilities.maxToolCalls);
    }

    // 不接受裸文本收尾的客户端（Roo/Kilo）要求最终回答是一个虚拟工具调用。
    // 工具名来自 IR，适配层不再硬编码客户端字符串。
    // 降级细节：原文本被放入 arguments.result 字段，并清空 result.text——
    // 文本已完整搬进工具参数，若不清空客户端会看到重复内容。
    // 参数序列化关闭缩进，保持与上游 tool_call.arguments 的紧凑格式一致。
    if (result.toolCalls.empty() && capabilities.requiresCompletionTool()) {
        generation::ToolCallDone completion;
        completion.name = capabilities.completionToolName;
        completion.index = 0;
        Json::Value args(Json::objectValue);
        args["result"] = result.text;
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        completion.arguments = Json::writeString(writer, args);
        result.toolCalls.push_back(std::move(completion));
        result.text.clear();
    }

    // 不允许文本与工具调用共存的客户端：丢弃伴随散文。
    // 取舍说明：这类客户端的状态机会因「文本 + tool_calls」同时出现而错乱，
    // 宁可丢失解释性文字也要保证工具调用可被正确执行。
    if (!result.toolCalls.empty() && !capabilities.allowsProseWithToolCall) {
        result.text.clear();
    }
    return result;
}

} // namespace actionproto
