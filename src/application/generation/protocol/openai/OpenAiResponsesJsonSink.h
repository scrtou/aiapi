#ifndef OPENAI_RESPONSES_JSON_SINK_H
#define OPENAI_RESPONSES_JSON_SINK_H

#include <application/generation/contracts/IResponseSink.h>
#include <json/json.h>
#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace generation::protocol::openai {

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
class OpenAiResponsesJsonSink : public IResponseSink, public IResponsePersistenceSink {
public:
    using ResponseCallback = std::function<void(const Json::Value&, int statusCode)>;

    /**
     * @brief 构造函数
     *
     * @param responseCallback 响应完成时的回调
     * @param model 模型名称
     * @param inputTokensEstimated 输入 token 估算（可选，用于 usage 兜底）
     */
    OpenAiResponsesJsonSink(
        ResponseCallback responseCallback,
        const std::string& model,
        int inputTokensEstimated = 0,
        bool nativeToolItems = false
    );

    ~OpenAiResponsesJsonSink() override = default;

    void onEvent(const generation::GenerationEvent& event) override;
    void onClose() override;
    bool isValid() const override;
    std::string getSinkType() const override { return "OpenAiResponsesJsonSink"; }

    const std::string& getCollectedText() const { return collectedText_; }
    std::optional<ResponsePersistenceRecord> responseRecord() const override
    {
        return responseRecord_;
    }

private:
    Json::Value buildResponse();

    ResponseCallback responseCallback_;
    std::string responseId_;
    std::string model_;

    int64_t createdAt_ = 0;

    std::string collectedText_;
    std::vector<generation::ToolCallDone> toolCalls_;

    std::optional<generation::Usage> usage_;
    Json::Value meta_{Json::objectValue};
    int inputTokensEstimated_ = 0;
    bool nativeToolItems_ = false;

    int statusCode_ = 200;
    bool hasError_ = false;
    platform::ErrorCode errorCode_ = platform::ErrorCode::Internal;
    std::string errorMessage_;

    bool closed_ = false;
    std::optional<ResponsePersistenceRecord> responseRecord_;
};

}  // namespace generation::protocol::openai

#endif // 头文件保护结束
