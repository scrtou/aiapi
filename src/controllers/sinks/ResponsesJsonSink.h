#ifndef RESPONSES_JSON_SINK_H
#define RESPONSES_JSON_SINK_H

#include <sessionManager/contracts/IResponseSink.h>
#include <json/json.h>
#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

/**
 * @brief Responses API JSON 输出 Sink
 *
 * 将 GenerationEvent 收集并转换为 OpenAI Responses API 的 JSON 响应。
 * 用于非流式响应（stream=false）。
 *
 * 事件处理：
 * - OutputTextDelta/Done -> 收集最终输出文本
 * - ToolCallDone -> 收集 tool_calls
 * - Completed.usage -> 记录 usage（如果有）
 * - Error -> 构建 error 响应并设置 HTTP 状态码
 */
class ResponsesJsonSink : public IResponseSink {
public:
    using ResponseCallback = std::function<void(const Json::Value&, int statusCode)>;

    /**
     * @brief 构造函数
     *
     * @param responseCallback 响应完成时的回调
     * @param model 模型名称
     * @param inputTokensEstimated 输入 token 估算（可选，用于 usage 兜底）
     */
    ResponsesJsonSink(
        ResponseCallback responseCallback,
        const std::string& model,
        int inputTokensEstimated = 0
    );

    ~ResponsesJsonSink() override = default;

    void onEvent(const generation::GenerationEvent& event) override;
    void onClose() override;
    bool isValid() const override;
    std::string getSinkType() const override { return "ResponsesJsonSink"; }

    const std::string& getCollectedText() const { return collectedText_; }

private:
    Json::Value buildResponse();

    ResponseCallback responseCallback_;
    std::string responseId_;
    std::string model_;

    int64_t createdAt_ = 0;

    std::string collectedText_;
    std::vector<generation::ToolCallDone> toolCalls_;

    std::optional<generation::Usage> usage_;
    int inputTokensEstimated_ = 0;

    int statusCode_ = 200;
    bool hasError_ = false;
    std::string errorMessage_;
    std::string errorType_;

    bool closed_ = false;
};

#endif // 头文件保护结束
