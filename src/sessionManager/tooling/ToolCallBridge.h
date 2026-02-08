#ifndef TOOL_CALL_BRIDGE_H
#define TOOL_CALL_BRIDGE_H

#include <string>
#include <vector>
#include <memory>
#include <json/json.h>
#include "sessionManager/contracts/GenerationRequest.h"
namespace toolcall {

/**
 * @brief Tool Call 事件类型
 * 
 * 用于表示解析过程中产生的事件
 */
enum class EventType {
    Text,           // 普通文本 delta
    ToolCallBegin,  // 工具调用开始
    ToolCallArgsDelta,  // 工具调用参数增量
    ToolCallEnd,    // 工具调用结束
    Error           // 解析错误
};

/**
 * @brief Tool Call 事件
 * 
 * 解析器产出的事件结构
 */
struct ToolCallEvent {
    EventType type = EventType::Text;
    std::string text;           // 类型时的文本内容
    std::string toolCallId;     // 工具调用 ID
    std::string toolName;       // 工具名称
    std::string argumentsDelta; // 参数增量 (JSON 片段)
    std::string errorMessage;   // 错误信息
};

/**
 * @brief Tool Call 结构
 * 
 * 完整的工具调用表示
 */
struct ToolCall {
    std::string id;             // 工具调用 ID
    std::string name;           // 工具名称
    std::string arguments;      // 参数 JSON 字符串
    

    static ToolCall fromJson(const Json::Value& json);

    Json::Value toJson() const;
};

/**
 * @brief Tool Result 结构
 * 
 * 工具执行结果表示
 */
struct ToolResult {
    std::string toolCallId;     // 对应的工具调用 ID
    std::string content;        // 结果内容
    bool isError = false;       // 是否为错误结果
    

    static ToolResult fromJson(const Json::Value& json);

    Json::Value toJson() const;
};

/**
 * @brief 桥接模式
 * 
 * 决定如何处理 tool calls
 */
enum class BridgeMode {
    Native,     // 上游原生支持 工具调用，直接透传
    TextBridge  // 上游不支持 工具调用，需要转换为文本协议
};

/**
 * @brief 通道能力
 * 
 * 描述通道的功能特性
 */
struct ChannelCapabilities {
    bool supportsToolCalls = true;      // 是否支持工具调用
    bool supportsStreamingToolCalls = true;  // 是否支持流式工具调用
    int maxToolPayloadBytes = 0;        // 最大工具载荷大小 (0=无限制)
    
    // 根据能力决定桥接模式
    BridgeMode getBridgeMode() const {
        return supportsToolCalls ? BridgeMode::Native : BridgeMode::TextBridge;
    }
};

/**
 * @brief 文本协议编解码器接口
 * 
 * 用于在不支持 tool calls 的通道上进行转换
 */
class IToolCallTextCodec {
public:
    virtual ~IToolCallTextCodec() = default;
    
    /**
     * @brief 将工具调用编码为文本
     * @param toolCall 工具调用结构
     * @return 编码后的文本
     */
    virtual std::string encodeToolCall(const ToolCall& toolCall) = 0;
    
    /**
     * @brief 将工具结果编码为文本
     * @param result 工具结果
     * @return 编码后的文本
     */
    virtual std::string encodeToolResult(const ToolResult& result) = 0;
    
    /**
     * @brief 生成工具定义的提示词
     * @param tools 工具定义列表
     * @return 注入到 system prompt 的文本
     */
    virtual std::string encodeToolDefinitions(const Json::Value& tools) = 0;
    
    /**
     * @brief 增量解析文本中的工具调用
     * @param chunk 输入的文本片段
     * @param events 输出的事件列表
     * @return 是否需要更多数据
     */
    virtual bool decodeIncremental(const std::string& chunk, std::vector<ToolCallEvent>& events) = 0;
    
    /**
     * @brief 刷新解析器缓冲区
     * @param events 输出的最终事件列表
     */
    virtual void flush(std::vector<ToolCallEvent>& events) = 0;
    
    /**
     * @brief 重置解析器状态
     */
    virtual void reset() = 0;
};

/**
 * @brief ToolCallBridge 核心类
 * 
 * 根据通道能力，选择执行请求/响应的转换
 */
class ToolCallBridge {
public:
    /**
     * @brief 构造函数
     * @param capabilities 通道能力
     */
    explicit ToolCallBridge(const ChannelCapabilities& capabilities);
    
    /**
     * @brief 获取桥接模式
     */
    BridgeMode getMode() const { return capabilities_.getBridgeMode(); }
    
    /**
     * @brief 是否需要进行转换
     */
    bool needsTransform() const { return getMode() == BridgeMode::TextBridge; }
    
    // ========== 请求侧转换 ==========
    
    /**
     * @brief 转换请求
     * 
     * 当上游不支持 tool calls 时：
     * - 将 tool definitions 注入到 system prompt
     * - 将 tool calls 消息转换为文本格式
     * 
     * @param messages 原始消息列表
     * @param tools 工具定义
     * @param systemPrompt 系统提示词（会被修改）
     * @return 转换后的消息列表
     */
    std::vector<Message> transformRequest(
        const std::vector<Message>& messages,
        const Json::Value& tools,
        std::string& systemPrompt
    );
    
    /**
     * @brief 转换单个消息中的 tool calls
     * @param toolCalls 工具调用列表
     * @return 转换后的文本
     */
    std::string transformToolCallsToText(const std::vector<ToolCall>& toolCalls);
    
    /**
     * @brief 转换 tool result 消息
     * @param result 工具结果
     * @return 转换后的文本
     */
    std::string transformToolResultToText(const ToolResult& result);
    
    // ========== 响应侧转换 ==========
    
    /**
     * @brief 处理响应文本块（流式）
     * 
     * 当上游不支持 tool calls 时，解析文本中的工具调用标签
     * 
     * @param chunk 输入的文本片段
     * @param events 输出的事件列表
     */
    void transformResponseChunk(
        const std::string& chunk,
        std::vector<ToolCallEvent>& events
    );
    
    /**
     * @brief 刷新响应解析器
     * @param events 输出的最终事件列表
     */
    void flushResponse(std::vector<ToolCallEvent>& events);
    
    /**
     * @brief 重置响应解析器状态
     */
    void resetResponseParser();
    
    // ========== 工具方法 ==========
    
    /**
     * @brief 设置文本编解码器
     * @param codec 编解码器实例
     */
    void setTextCodec(std::shared_ptr<IToolCallTextCodec> codec);
    
    /**
     * @brief 获取当前编解码器
     */
    std::shared_ptr<IToolCallTextCodec> getTextCodec() const { return codec_; }
    
private:
    ChannelCapabilities capabilities_;
    std::shared_ptr<IToolCallTextCodec> codec_;
};

/**
 * @brief 创建默认的 ToolCallBridge
 * @param supportsToolCalls 是否支持工具调用
 * @return ToolCallBridge 实例
 */
std::shared_ptr<ToolCallBridge> createToolCallBridge(bool supportsToolCalls);

} // 命名空间结束

#endif // 头文件保护结束
