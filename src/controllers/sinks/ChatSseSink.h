#ifndef CHAT_SSE_SINK_H
#define CHAT_SSE_SINK_H

#include <sessionManager/IResponseSink.h>
#include <drogon/drogon.h>
#include <functional>
#include <optional>
#include <string>

using namespace drogon;

/**
 * @brief Chat Completions SSE 输出 Sink
 * 
 * 将 GenerationEvent 转换为 OpenAI Chat Completions SSE 格式输出。
 * 
 * SSE 格式说明：
 * - 第一条包含 role=assistant
 * - 后续 delta.content=chunk
 * - 最后发 finish_reason=stop + [DONE]
 * 
 * 参考设计文档: plans/aiapi-refactor-design.md 第 7.1 节
 */
class ChatSseSink : public IResponseSink {
public:
    using StreamCallback = std::function<bool(const std::string&)>;
    using CloseCallback = std::function<void()>;
    
    /**
     * @brief 构造函数
     * 
     * @param streamCallback 用于发送 SSE 数据的回调
     * @param closeCallback 关闭连接的回调
     * @param model 模型名称
     */
    ChatSseSink(
        StreamCallback streamCallback,
        CloseCallback closeCallback,
        const std::string& model
    );
    
    ~ChatSseSink() override = default;
    
    void onEvent(const generation::GenerationEvent& event) override;
    void onClose() override;
    bool isValid() const override;
    std::string getSinkType() const override { return "ChatSseSink"; }
    
private:
    /**
     * @brief 发送 SSE 事件
     * 
     * @param data 事件数据（JSON 字符串）
     */
    void sendSseEvent(const std::string& data);
    
    /**
     * @brief 发送 [DONE] 信号
     */
    void sendDone();
    
    /**
     * @brief 生成 chunk 响应 JSON
     * 
     * @param delta 增量内容
     * @param finishReason 完成原因（可选）
     * @param includeRole 是否包含 role 字段（第一条需要）
     */
    std::string buildChunkJson(
        const std::string& delta,
        const std::string& finishReason = "",
        bool includeRole = false
    );

    /**
     * @brief 生成包含 usage 的 chunk 响应 JSON（choices 为空数组）
     */
    std::string buildUsageChunkJson(const generation::Usage& usage);

    /**
     * @brief 生成包含 tool_calls 的 chunk 响应 JSON
     */
    std::string buildToolCallChunkJson(
        const generation::ToolCallDone& toolCall,
        const std::string& finishReason = "",
        bool includeRole = false
    );
    
    /**
     * @brief 生成唯一的 completion ID
     */
    static std::string generateCompletionId();
    
    StreamCallback streamCallback_;
    CloseCallback closeCallback_;
    std::string model_;
    std::string completionId_;
    bool firstChunk_ = true;
    bool sentText_ = false;
    std::optional<generation::Usage> usage_;
    bool closed_ = false;
};

#endif // CHAT_SSE_SINK_H
