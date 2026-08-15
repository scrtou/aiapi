#pragma once

#include <application/generation/contracts/IResponseSink.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace generation::protocol::claude {

class ClaudeJsonSink final : public IResponseSink
{
  public:
    using ResponseCallback = std::function<void(const Json::Value&, int)>;

    ClaudeJsonSink(ResponseCallback callback,
                   std::string model,
                   int inputTokensEstimated = 0);

    void onEvent(const generation::GenerationEvent& event) override;
    void onClose() override;
    bool isValid() const override;
    std::string getSinkType() const override { return "ClaudeJsonSink"; }

  private:
    Json::Value buildResponse() const;
    static std::string generateMessageId();

    ResponseCallback callback_;
    std::string model_;
    std::string messageId_;
    std::string text_;
    std::vector<generation::ToolCallDone> toolCalls_;
    std::string stopReason_ = "end_turn";
    std::optional<generation::Usage> usage_;
    int statusCode_ = 200;
    bool hasError_ = false;
    platform::ErrorCode errorCode_ = platform::ErrorCode::Internal;
    std::string errorMessage_;
    int inputTokensEstimated_ = 0;
    bool closed_ = false;
};

}  // namespace generation::protocol::claude
