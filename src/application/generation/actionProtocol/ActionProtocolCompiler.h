// ============================================================================
// ActionProtocolCompiler —— action 协议编译器与客户端能力 IR
//
// 职责边界：
//   1. 将上游模型输出（action-v3 JSON / action-v2 XML）编译为统一的
//      ActionEnvelope，供 core 层转换成 OpenAI 兼容的工具调用事件。
//   2. 承载「客户端能力 IR」（ClientCapabilities）：把各客户端
//      （Roo/Kilo、Codex、ClaudeCode 等）的协议差异集中描述为一组
//      布尔/数值字段。
//
// 重要约定（P3 重构后的硬性规则）：
//   clientType 原始字符串在整个代码库中**只允许**在 normalizeClientType()
//   内被解析一次。任何其它模块若再出现 `clientType == "..."` 形式的比较，
//   都属于回归——应改为读取 ClientCapabilities 的对应字段，必要时在此文件
//   新增能力位。这样做的收益：
//     - 客户端名的大小写 / 连字符变体只需在一处收敛；
//     - 新接入一个客户端时只改本文件的家族矩阵，不必扫描调用点；
//     - 每个行为差异都有具名字段，语义自解释而非隐藏在字符串里。
// ============================================================================

#ifndef AIAPI_ACTION_PROTOCOL_COMPILER_H
#define AIAPI_ACTION_PROTOCOL_COMPILER_H

#include <json/json.h>
#include <cstddef>
#include <string>
#include <vector>

namespace actionproto {

// 线格式（wire format）：模型回包的外层封装形态。
//   Auto   —— 不预设，按首个非空白字符嗅探（'{' 视为 JSON，否则按 XML 解析）
//   JsonV3 —— 强制 action-v3：哨兵之后必须紧跟单个 JSON 对象
//   XmlV2  —— 强制 action-v2：<action_protocol version="1"> ... <end_action/>
// 强制模式用于已知客户端偏好（见 ClientCapabilities::prefersXmlWire /
// ClientCapabilities::prefersJsonWire），
// 可在格式不符时尽早失败，避免把 XML 误当 JSON 解析产生歧义诊断。
enum class WireFormat {
    Auto,
    JsonV3,
    XmlV2
};

// 一轮 action 的语义类别：要么继续调用工具，要么以最终答复收尾。
// 二者互斥，由 ActionEnvelope::kind() 依据 toolCalls 是否为空推导。
enum class ActionKind {
    ToolCall,
    FinalResponse
};

// 单个工具调用。argumentsJson 保持为**未解析的 JSON 文本**：
// 下游需按 OpenAI 规范原样透传到 tool_call.function.arguments，
// 中途反序列化再序列化会丢失键序并可能改变数字字面量表示。
struct ToolAction {
    std::string id;            // 工具调用 ID；为空时由 core 层补发
    std::string name;          // 工具名，须命中客户端已声明的工具表
    std::string argumentsJson; // 参数对象的原始 JSON 文本
};

// 编译产物：一轮响应的规范化表示，是编译器与 core 层之间的唯一契约。
// toolCalls 与 finalResponse 语义上互斥——同时非空视为协议违规，
// 由 compileResponse 阶段拦截而非留给下游猜测。
struct ActionEnvelope {
    int protocolVersion = 1;              // action 协议版本，当前恒为 1
    std::string nonce;                    // 每请求哨兵携带的随机串，用于防重放/串话
    std::vector<ToolAction> toolCalls;    // 工具调用序列（可能被能力 IR 截断）
    std::string finalResponse;            // 最终文本答复；严格客户端下会被降级为虚拟工具调用

    ActionKind kind() const {
        return !toolCalls.empty() ? ActionKind::ToolCall : ActionKind::FinalResponse;
    }
};

// 客户端族群的规范化标识。clientType 字符串只允许在 normalizeClientType
// 中被解析一次，之后系统内部一律以 ClientFamily / ClientCapabilities 流转。
enum class ClientFamily {
    Generic,    // 标准 OpenAI 兼容客户端，无特殊约束
    RooKilo,    // RooCode / Kilo-Code：每轮必须恰好 1 个工具调用，收尾走 attempt_completion
    Codex,      // Codex CLI：支持自定义工具；未开并行时需把多调用收敛为首个
    ClaudeCode  // Claude Code：支持自定义工具；收到 tool_calls 后停止消费同轮文本
};

const char* clientFamilyName(ClientFamily family);
ClientFamily normalizeClientType(const std::string& clientType);

// 完整的客户端能力 IR。所有下游模块（tooling / core / adapter）都必须
// 读取本结构体的字段，而不是重新比较 clientType 字符串。
struct ClientCapabilities {
    ClientFamily family = ClientFamily::Generic;

    // --- action 语义：一轮响应「必须/可以」产出什么 ---
    // 每轮都必须给出 action，不允许空转。为 true 时若模型未产出任何工具调用，
    // 适配层会把文本包装成 completionToolName 以满足客户端状态机。
    bool requiresActionEveryTurn = false;
    // 是否接受裸文本收尾。false 表示客户端只消费工具调用事件，
    // 直接下发纯文本会导致其会话卡死。
    bool supportsFinalText = true;
    // 是否允许一轮内出现多个工具调用（通常映射请求的 parallel_tool_calls）。
    bool supportsParallelCalls = false;
    // 是否支持超出标准工具表的自定义工具（freeform / custom tool）。
    bool supportsCustomTools = false;
    // 一轮内允许的工具调用上限。与 coalescesParallelCalls 配合使用：
    // 超限时截断而非报错，保证客户端只接收其声明数量内的调用。
    size_t maxToolCalls = 1;

    // --- 收尾语义 ---
    // 非空表示该客户端不接受裸文本收尾，final_response 必须被降级为
    // 名为 completionToolName 的虚拟工具调用（Roo/Kilo 的 attempt_completion）。
    std::string completionToolName;

    // --- 传输与产出约束 ---
    bool prefersXmlWire = false;          // 默认走 XML action-v2 而非 JSON action-v3
    bool prefersJsonWire = false;         // 默认走 JSON action-v3 而非 XML action-v2
    bool allowsProseWithToolCall = true;  // 是否允许文本与工具调用同时出现
    bool requiresStrictToolNames = false; // 工具名必须严格命中已声明工具表，未命中即丢弃
    // 上游给出的调用数超过 maxToolCalls 时，截断为前 maxToolCalls 个而非报错。
    // 典型场景：Codex 在 parallel_tool_calls=false 下只接受首个调用。
    bool coalescesParallelCalls = false;
    // 客户端在收到 finish_reason="tool_calls" 后即停止消费同轮文本，
    // 因此零宽会话 ID 等带外文本必须在工具调用事件之前先行发出。
    bool stopsConsumingTextAfterToolCall = false;

    // Codex 与 Claude Code 在没有原生工具通道时共用同一套严格文本桥接
    // 策略（冲突指令清理、精确哨兵、协议重试与动作收尾）。
    bool requiresStrictToolBridge = false;

    // 是否需要把 final_response 降级为虚拟工具调用。
    bool requiresCompletionTool() const {
        return !completionToolName.empty();
    }

    // 「严格工具客户端」：每轮必须有 action，且收尾也只能借工具调用表达。
    // 这是 Roo/Kilo 系列的唯一判定入口，避免分散比较客户端名称。
    // 注意：此处是**推导**而非独立开关，避免出现两个字段互相矛盾的状态。
    bool isStrictToolClient() const {
        return requiresActionEveryTurn && requiresCompletionTool();
    }
};

// 能力 IR 的两个入口。业务代码通常调用前者（直接吃请求里的 clientType），
// 后者供已知家族或单测按矩阵构造使用。parallelToolCalls 来自请求的
// parallel_tool_calls 字段：它会影响 maxToolCalls 与 coalescesParallelCalls，
// 因此能力 IR 是「客户端 × 请求参数」的函数，不能缓存成全局常量。
ClientCapabilities capabilitiesForClient(const std::string& clientType,
                                          bool parallelToolCalls);
ClientCapabilities capabilitiesForFamily(ClientFamily family,
                                          bool parallelToolCalls);

// 单次编译的输入约束。
struct CompileOptions {
    // 期望的哨兵前缀（每请求随机）。非空时响应必须以它开头，
    // 用于隔离串话与模型自行伪造的协议块；为空表示跳过该校验。
    std::string expectedSentinel;
    ClientCapabilities capabilities;              // 目标客户端能力 IR
    WireFormat wireFormat = WireFormat::Auto;     // 期望线格式，Auto 时嗅探
};

enum class CompileError {
    None,
    MissingSentinel,
    InvalidEnvelope,
    InvalidActionShape,
    InvalidArgumentsJson,
    MultipleActions,
    MissingAction
};

struct CompileDiagnostic {
    CompileError code = CompileError::None;
    std::string message;
    size_t offset = 0;
};

struct CompileResult {
    bool matched = false;
    bool valid = false;
    ActionEnvelope envelope;
    CompileDiagnostic diagnostic;
};

class ActionProtocolCompiler {
public:
    static CompileResult compileResponse(const std::string& input,
                                         const CompileOptions& options);

    // 生成供不支持原生 tool calls 的上游模型使用的 JSON-only action-v3
    // 规则。compileResponse 同时支持已声明的 action-v2 XML wire format。
    static std::string buildRouterPolicy(
        const std::string& sentinel,
        const ClientCapabilities& capabilities);
};

} // namespace actionproto

#endif
