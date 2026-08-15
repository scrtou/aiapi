#pragma once

#include <application/generation/contracts/GenerationSession.h>
#include <application/generation/protocol/common/ProtocolContracts.h>
#include <domain/port/IAccountSettingsQuery.h>

namespace generation::protocol::openai {

enum class OpenAiOperation {
    ChatCompletions,
    ResponsesCreate,
};

/** OpenAI wire payload to canonical GenerationRequest adapter. */
class OpenAiRequestAdapter final : public IProtocolRequestAdapter
{
  public:
    explicit OpenAiRequestAdapter(OpenAiOperation operation)
        : operation_(operation)
    {
    }

    AdapterResult adapt(const RawProtocolRequest& raw) const override;

    static void setTrackingMode(SessionTrackingMode mode) { trackingMode_ = mode; }
    static void setAccountSettingsQuery(IAccountSettingsQuery* query)
    {
        accountSettings_ = query;
    }

  private:
    static GenerationRequest buildChatRequest(
        const Json::Value& requestBody,
        const aiapi::RequestHeaders& headers);
    static GenerationRequest buildResponsesRequest(
        const Json::Value& requestBody,
        const aiapi::RequestHeaders& headers);
    static Json::Value extractClientInfo(const aiapi::RequestHeaders& headers);
    static void parseChatMessages(
        const Json::Value& messages,
        std::vector<Message>& result,
        std::string& systemPrompt,
        std::string& currentInput,
        std::vector<ImageInfo>& images);
    static void parseResponseInput(
        const Json::Value& input,
        std::vector<Message>& messages,
        std::string& systemPrompt,
        std::string& currentInput,
        std::vector<ImageInfo>& images);
    static void parseResponseInputItems(
        const Json::Value& inputItems,
        std::string& currentInput,
        std::vector<ImageInfo>& images);
    static std::string extractContentText(
        const Json::Value& content,
        std::vector<ImageInfo>& images,
        bool stripZeroWidth = false,
        std::vector<std::string>* outRawTexts = nullptr);
    static ImageInfo parseImageUrl(const std::string& url);

    OpenAiOperation operation_;
    static SessionTrackingMode trackingMode_;
    static IAccountSettingsQuery* accountSettings_;
};

}  // namespace generation::protocol::openai
