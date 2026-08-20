#ifndef GENERATION_REQUEST_H
#define GENERATION_REQUEST_H

#include <string>
#include <vector>
#include <optional>
#include <map>
#include <utility>
#include <json/json.h>
#include <domain/model/ImageInfo.h>
#include <application/generation/contracts/CurrentTurnKind.h>

/** Protocol-neutral response identity and persistence lifecycle. */
/** 响应生命周期 */
enum class ResponseLifecycle {
    Immediate,
    Stored,
};

/**
 * @brief 消息角色
 */
enum class MessageRole {
    System,
    User,
    Assistant,
    Tool
};

/** Protocol-neutral content semantics used throughout the application core. */
enum class ContentBlockType {
    Text,
    Image,
    ToolUse,
    ToolResult,
    Thinking,
};

struct ContentBlock {
    ContentBlockType type = ContentBlockType::Text;
    std::string text;
    std::string imageUrl;
    std::string mediaType;
    std::string toolCallId;
    std::string toolName;
    Json::Value toolInput;
    std::string toolInputDelta;
    std::string toolResult;
    bool toolResultIsError = false;
};

enum class ToolDefinitionKind {
    Function,
    Custom,
};

struct ToolDefinition {
    std::string name;
    std::string description;
    Json::Value inputSchema;
    ToolDefinitionKind kind = ToolDefinitionKind::Function;
    std::string originalName;
    std::string namespacePath;
};

enum class ToolChoiceMode {
    Auto,
    None,
    Any,
    Specific,
};

struct ToolChoice {
    ToolChoiceMode mode = ToolChoiceMode::Auto;
    std::optional<std::string> toolName;
};

/**
 * A provider-facing fragment of the current turn.
 *
 * The protocol adapter owns the rendered text because upstream bridge syntax
 * is protocol-specific.  The optional tool-result identity is canonical,
 * however, so the session lifecycle can suppress an already-consumed result
 * without parsing or rewriting protocol text.
 */
struct CurrentInputPart {
    std::string text;
    std::string toolResultCallId;
    bool isToolResult = false;
    // Some clients send an append-only transcript in the current input rather
    // than only the new user text.  Such adapters opt in so the session core
    // can suppress the already-delivered text prefix after continuity is
    // resolved.
    bool isReplayableText = false;
    // Auxiliary client prompts (for example a suggestion/recap request) may
    // contain that prefix but must not replace the durable user-text snapshot.
    bool isAuxiliary = false;
};

struct GenerationCapabilities {
    bool text = true;
    bool images = false;
    bool streaming = false;
    bool tools = false;
    bool parallelTools = false;
    bool reasoning = false;
    bool continuity = false;

    static GenerationCapabilities all() { return GenerationCapabilities{
        true, true, true, true, true, true, true}; }
};

inline GenerationCapabilities intersectCapabilities(const GenerationCapabilities& protocol,
                                                    const GenerationCapabilities& model,
                                                    const GenerationCapabilities& provider)
{
    GenerationCapabilities result;
    result.text = protocol.text && model.text && provider.text;
    result.images = protocol.images && model.images && provider.images;
    result.streaming = protocol.streaming && model.streaming && provider.streaming;
    result.tools = protocol.tools && model.tools && provider.tools;
    result.parallelTools = protocol.parallelTools && model.parallelTools && provider.parallelTools;
    result.reasoning = protocol.reasoning && model.reasoning && provider.reasoning;
    result.continuity = protocol.continuity && model.continuity && provider.continuity;
    return result;
}

/**
 * @brief 消息结构
 *
 * 内部 canonical 强类型消息表示
 */
struct Message {
    MessageRole role = MessageRole::User;
    std::vector<ContentBlock> blocks;

    bool hasToolUses() const {
        for (const auto& block : blocks) {
            if (block.type == ContentBlockType::ToolUse) return true;
        }
        return false;
    }

    std::string toolResultCallId() const {
        for (const auto& block : blocks) {
            if (block.type == ContentBlockType::ToolResult &&
                !block.toolCallId.empty()) {
                return block.toolCallId;
            }
        }
        return "";
    }
    
    // 便捷构造函数
    static Message user(const std::string& text) {
        Message msg;
        msg.role = MessageRole::User;
        ContentBlock block;
        block.type = ContentBlockType::Text;
        block.text = text;
        msg.blocks.push_back(std::move(block));
        return msg;
    }
    
    static Message assistant(const std::string& text) {
        Message msg;
        msg.role = MessageRole::Assistant;
        ContentBlock block;
        block.type = ContentBlockType::Text;
        block.text = text;
        msg.blocks.push_back(std::move(block));
        return msg;
    }
    
    static Message system(const std::string& text) {
        Message msg;
        msg.role = MessageRole::System;
        ContentBlock block;
        block.type = ContentBlockType::Text;
        block.text = text;
        msg.blocks.push_back(std::move(block));
        return msg;
    }
    
    // 获取纯文本内容
    std::string getTextContent() const {
        std::string result;
        for (const auto& block : blocks) {
            if (block.type == ContentBlockType::Text ||
                block.type == ContentBlockType::Thinking) {
                result += block.text;
            } else if (block.type == ContentBlockType::ToolResult) {
                result += block.toolResult;
            }
        }
        return result;
    }
};

/**
 * @brief 图片信息
 *
 * 当前请求中的图片列表
 */
// ImageInfo 已迁至 domain/model/ImageInfo.h（见文件头 include）

namespace continuity {

inline bool hasStableClientSession(const Json::Value& clientInfo) {
    return clientInfo.isObject() &&
           clientInfo.get("client_type", "").asString() == "Codex" &&
           clientInfo.isMember("client_session_id") &&
           clientInfo["client_session_id"].isString() &&
           !clientInfo["client_session_id"].asString().empty();
}

} // namespace continuity

/**
 * @brief 统一生成请求
 *
 * 统一"业务语义"，而不是强行对齐 HTTP 字段。
 * Chat Completions 和 Responses API 都映射到这个结构。
 *
 * 参考设计文档: plans/aiapi-refactor-design.md 第 4.1 节
 */
struct GenerationRequest {
    // ========== 入口/会话连续性 ==========
    ResponseLifecycle responseLifecycle = ResponseLifecycle::Immediate;
    std::optional<std::string> previousResponseId;   // // Responses： previous_响应_id（可选）
    std::vector<std::string> continuityTexts;        // 原始文本集合（用于 ZeroWidth / 其它续聊解析，保留零宽字符）
    Json::Value clientInfo;                          // 后续可换成强类型
    
    // ========== 生成目标 ==========
    std::string provider;           // 如 "chaynsapi"
    std::string model;              // 模型名称
    std::string systemPrompt;       // 系统提示词
    
    // ========== 输入 ==========
    std::vector<Message> messages;  // 内部强类型消息列表
    std::string currentInput;       // 当前用户输入（纯文本）
    // Adapters use this semantic marker for client-side requests such as
    // title/recap/suggestion generation. Auxiliary turns are still sent to
    // the provider, but must not become durable conversation history.
    CurrentTurnKind currentTurnKind = CurrentTurnKind::Durable;
    // Optional lossless decomposition of currentInput.  Adapters that carry
    // tool results populate this so the core can reconcile result IDs after
    // session continuity has been resolved.
    std::vector<CurrentInputPart> currentInputParts;
    std::vector<ImageInfo> images;  // 当前请求中的图片列表
    
    // ========== 工具调用 ==========
    std::vector<ToolDefinition> toolDefinitions;
    ToolChoice toolChoiceSpec;
    bool parallelToolCalls = true;   // 是否允许同一轮返回多个工具调用
    
    // ========== 输出要求 ==========
    bool stream = false;            // 是否流式输出
    std::optional<int> maxOutputTokens;
    std::optional<double> temperature;
    std::optional<double> topP;

    // Capability negotiation is performed at the application boundary. The
    // core consumes only the effective result and never re-reads protocol
    // JSON to infer support.
    GenerationCapabilities protocolCapabilities = GenerationCapabilities::all();
    GenerationCapabilities modelCapabilities = GenerationCapabilities::all();
    GenerationCapabilities providerCapabilities = GenerationCapabilities::all();
    GenerationCapabilities effectiveCapabilities = GenerationCapabilities::all();
    bool modelCapabilitiesDeclared = false;
    std::vector<std::string> capabilityDegradations;

    // Protocol-specific data is retained at the boundary and must not be
    // interpreted by GenerationPipeline.
    Json::Value protocolExtensions{Json::objectValue};
    
    // ========== 追踪 ==========
    std::string requestId;          // 请求 ID（可选）
    std::string traceId;            // 追踪 ID（可选）
    
    // ========== 辅助方法 ==========
    
    /** Whether this request participates in stored response-ID continuity. */
    bool usesStoredResponseLifecycle() const {
        return responseLifecycle == ResponseLifecycle::Stored;
    }
    
    /**
     * @brief 获取所有消息的纯文本内容（用于日志等）
     */
    std::string getMessagesText() const {
        std::string result;
        for (const auto& msg : messages) {
            result += msg.getTextContent() + "\n";
        }
        return result;
    }
};

#endif // 头文件保护结束
