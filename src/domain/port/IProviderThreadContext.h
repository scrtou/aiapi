#pragma once

#include <platform/result/Result.h>

#include <string>

namespace provider {

/**
 * Provider-owned upstream conversation state.
 *
 * Session/application code supplies stable local conversation IDs but never
 * sees a provider's upstream IDs or account credentials.  Cleanup and rebind
 * are explicit operations rather than side effects on session_st.
 */
class IProviderThreadContext
{
  public:
    virtual ~IProviderThreadContext() = default;

    virtual platform::Result<void> eraseThreadContext(
        const std::string& conversationId) = 0;

    virtual platform::Result<void> transferThreadContext(
        const std::string& oldConversationId,
        const std::string& newConversationId) = 0;

    virtual platform::Result<void> deleteUpstreamThread(
        const std::string& accountUserName,
        const std::string& threadId,
        const std::string& origin,
        const std::string& referer) = 0;
};

}  // namespace provider
