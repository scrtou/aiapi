#pragma once

#include <domain/model/ImageInfo.h>
#include <domain/model/ProviderCallContext.h>
#include <infrastructure/provider/chayns/ChaynsHttpTransport.h>
#include <infrastructure/provider/chayns/ChaynsProviderPolicy.h>
#include <platform/result/Result.h>

#include <json/json.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace chayns {

/**
 * Chayns wire-protocol adapter.
 *
 * The provider orchestrator decides *when* an image must be uploaded; this
 * class owns only multipart request construction and response decoding for
 * that upstream endpoint.  Keeping the transport injected makes the boundary
 * deterministic in contract tests.
 */
class ChaynsProtocolClient final
{
  public:
    struct MessageSubmission
    {
        bool transportOk = false;
        bool accepted = false;
        bool ambiguous = false;
        int statusCode = 0;
        std::string threadId;
        std::string userAuthorId;
        std::string agentAuthorId;
        std::string messageId;
        std::string creationTime;
        platform::Error error;
    };

    explicit ChaynsProtocolClient(std::shared_ptr<IChaynsHttpTransport> transport);

    [[nodiscard]] std::string uploadImage(
        const ImageInfo& image,
        const std::string& personId,
        const std::string& authToken,
        const std::string& accountUserName,
        const std::string& origin,
        const std::string& referer,
        const provider::ProviderCallContext& context,
        std::string_view requestId = {},
        std::string_view conversationId = {}) const;

    /** Fetch and decode one upstream message page used by the polling loop. */
    [[nodiscard]] std::optional<Json::Value> getThreadMessages(
        const std::string& threadId,
        const std::string& afterDate,
        const Accountinfo_st& account,
        const policy::RequestRoute& route,
        double timeoutSeconds,
        std::string_view requestId = {},
        std::string_view conversationId = {}) const;

    [[nodiscard]] std::optional<std::string> getPersonId(
        const Accountinfo_st& account,
        const policy::RequestRoute& route,
        const provider::ProviderCallContext& context,
        std::string_view requestId = {},
        std::string_view conversationId = {}) const;

    [[nodiscard]] MessageSubmission sendFollowupMessage(
        const std::string& threadId,
        const Json::Value& body,
        const Accountinfo_st& account,
        const policy::RequestRoute& route,
        const provider::ProviderCallContext& context,
        std::string_view requestId = {},
        std::string_view conversationId = {}) const;

    [[nodiscard]] MessageSubmission createThread(
        const Json::Value& body,
        const Accountinfo_st& account,
        const policy::RequestRoute& route,
        const std::string& userPersonId,
        const std::string& modelPersonId,
        const provider::ProviderCallContext& context,
        std::string_view requestId = {},
        std::string_view conversationId = {}) const;

    [[nodiscard]] std::optional<Json::Value> getModelCatalog(
        const provider::ProviderCallContext* context = nullptr) const;

    [[nodiscard]] platform::Result<void> deleteThread(
        const Accountinfo_st& account,
        const std::string& threadId,
        const std::string& origin,
        const std::string& referer) const;

  private:
    std::shared_ptr<IChaynsHttpTransport> m_transport;
};

}  // namespace chayns
