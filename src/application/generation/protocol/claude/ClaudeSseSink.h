#pragma once

#include <application/generation/contracts/IResponseSink.h>

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace generation::protocol::claude {

class ClaudeSseSink final : public IResponseSink
{
  public:
    using StreamCallback = std::function<bool(const std::string&)>;
    using CloseCallback = std::function<void()>;

    ClaudeSseSink(StreamCallback streamCallback,
                  CloseCallback closeCallback,
                  std::string model,
                  int inputTokensEstimated = 0);

    void onEvent(const generation::GenerationEvent& event) override;
    void onClose() override;
    bool isValid() const override;
    bool supportsIncrementalToolEvents() const override { return true; }
    std::string getSinkType() const override { return "ClaudeSseSink"; }

  private:
    void sendEvent(const std::string& type, const Json::Value& data);
    void sendError(const generation::Error& error);
    void ensureMessageStarted(const generation::Started* started = nullptr);
    void ensureTextBlock();
    void closeTextBlock();
    void closeToolBlock(const std::string& key);
    void finishMessage(const generation::Completed& completed);
    static std::string toolStateKey(const std::string& id, int index);
    static std::string generateMessageId();

    StreamCallback streamCallback_;
    CloseCallback closeCallback_;
    std::string model_;
    std::string messageId_;
    int inputTokensEstimated_ = 0;
    int outputTokensEstimated_ = 0;
    int nextContentIndex_ = 0;
    int textIndex_ = -1;
    std::unordered_map<std::string, int> toolIndexes_;
    std::unordered_set<std::string> startedTools_;
    std::unordered_set<std::string> completedTools_;
    std::unordered_map<std::string, std::unordered_set<int>> toolSequences_;
    std::string text_;
    std::optional<generation::Usage> usage_;
    bool messageStarted_ = false;
    bool textBlockOpen_ = false;
    bool messageFinished_ = false;
    bool closed_ = false;
};

}  // namespace generation::protocol::claude
