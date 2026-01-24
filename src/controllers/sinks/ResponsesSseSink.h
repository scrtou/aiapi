#ifndef RESPONSES_SSE_SINK_H
#define RESPONSES_SSE_SINK_H

#include <sessionManager/IResponseSink.h>
#include <drogon/drogon.h>
#include <functional>
#include <string>

using namespace drogon;

/**
 * @brief Responses API SSE 输出 Sink
 * 
 * 将 GenerationEvent 转换为 OpenAI Responses API SSE 事件序列。
 * 
 * 事件序列映射：
 * - Started → response.created + response.in_progress + response.output_item.added + response.content_part.added
 * - OutputTextDelta → response.output_text.delta
 * - OutputTextDone → response.output_text.done + response.content_part.done + response.output_item.done
 * - Completed → response.completed
 * - Error → response.failed
 * 
 * 参考设计文档: plans/aiapi-refactor-design.md 第 7.2 节
 */
class ResponsesSseSink : public IResponseSink {
public:
    using StreamCallback = std::function<bool(const std::string&)>;
    using CloseCallback = std::function<void()>;
    
    /**
     * @brief 构造函数
     * 
     * @param streamCallback 用于发送 SSE 数据的回调
     * @param closeCallback 关闭连接的回调
     * @param responseId Response ID
     * @param model 模型名称
     */
    ResponsesSseSink(
        StreamCallback streamCallback,
        CloseCallback closeCallback,
        const std::string& responseId,
        const std::string& model
    );
    
    ~ResponsesSseSink() override = default;
    
    void onEvent(const generation::GenerationEvent& event) override;
    void onClose() override;
    bool isValid() const override;
    std::string getSinkType() const override { return "ResponsesSseSink"; }
    
private:
    /**
     * @brief 发送 SSE 事件
     * 
     * @param eventType 事件类型 (如 "response.created")
     * @param data 事件数据 (JSON 字符串)
     */
    void sendSseEvent(const std::string& eventType, const std::string& data);
    
    /**
     * @brief 处理 Started 事件
     */
    void handleStarted(const generation::Started& event);
    
    /**
     * @brief 处理 OutputTextDelta 事件
     */
    void handleOutputTextDelta(const generation::OutputTextDelta& event);
    
    /**
     * @brief 处理 OutputTextDone 事件
     */
    void handleOutputTextDone(const generation::OutputTextDone& event);
    
    /**
     * @brief 处理 Completed 事件
     */
    void handleCompleted(const generation::Completed& event);
    
    /**
     * @brief 处理 Error 事件
     */
    void handleError(const generation::Error& event);
    
    /**
     * @brief 构建 Response 对象 JSON
     * 
     * @param status 状态 ("in_progress", "completed", "failed")
     */
    Json::Value buildResponseObject(const std::string& status);
    
    StreamCallback streamCallback_;
    CloseCallback closeCallback_;
    std::string responseId_;
    std::string model_;
    std::string outputText_;     // 累积的输出文本
    int outputItemIndex_ = 0;   // 当前输出项索引
    bool closed_ = false;
};

#endif // RESPONSES_SSE_SINK_H
