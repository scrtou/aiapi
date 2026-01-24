#ifndef CHAT_JSON_SINK_H
#define CHAT_JSON_SINK_H

#include <sessionManager/IResponseSink.h>
#include <json/json.h>
#include <functional>
#include <string>

/**
 * @brief Chat Completions JSON 输出 Sink
 * 
 * 将 GenerationEvent 收集并转换为 OpenAI Chat Completions JSON 格式输出。
 * 用于非流式响应。
 * 
 * 参考设计文档: plans/aiapi-refactor-design.md 第 7.1 节
 */
class ChatJsonSink : public IResponseSink {
public:
    using ResponseCallback = std::function<void(const Json::Value&, int statusCode)>;
    
    /**
     * @brief 构造函数
     * 
     * @param responseCallback 响应完成时的回调
     * @param model 模型名称
     */
    ChatJsonSink(
        ResponseCallback responseCallback,
        const std::string& model
    );
    
    ~ChatJsonSink() override = default;
    
    void onEvent(const generation::GenerationEvent& event) override;
    void onClose() override;
    bool isValid() const override;
    std::string getSinkType() const override { return "ChatJsonSink"; }
    
    /**
     * @brief 获取收集到的完整文本
     */
    const std::string& getCollectedText() const { return collectedText_; }
    
    /**
     * @brief 获取完成原因
     */
    const std::string& getFinishReason() const { return finishReason_; }
    
private:
    /**
     * @brief 构建最终的 Chat Completions JSON 响应
     */
    Json::Value buildResponse();
    
    /**
     * @brief 生成唯一的 completion ID
     */
    static std::string generateCompletionId();
    
    ResponseCallback responseCallback_;
    std::string model_;
    std::string completionId_;
    std::string collectedText_;
    std::string finishReason_ = "stop";
    int statusCode_ = 200;
    bool hasError_ = false;
    std::string errorMessage_;
    std::string errorType_;
    bool closed_ = false;
};

#endif // CHAT_JSON_SINK_H
