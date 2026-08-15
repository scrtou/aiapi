#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace chayns {

// The persisted form of the provider-owned upstream-thread context.  It is
// deliberately credential-free: the account database remains the sole owner
// of tokens and person identifiers.
struct ThreadLedgerRow
{
    std::string threadId;
    std::string sessionId;
    std::string userAuthorId;
    std::string agentAuthorId;
    std::string accountUserName;
    std::string modelId;
    std::string accountType;
    int threadTypeId = 8;
    std::int64_t workspaceUacId = 0;
    std::string origin;
    std::string referer;
    std::string lastRequestMessageId;
    std::string lastRequestCreationTime;
    std::string lastAssistantMessageId;
    std::int64_t createdAt = 0;
    std::int64_t lastActiveAt = 0;
    int deleteAttempts = 0;
};

/**
 * Narrow persistence seam used by the Chayns provider.
 *
 * The concrete DB manager also serves the reaper, but a provider needs only
 * the thread-context operations below.  Keeping this contract small lets the
 * offline provider fixture exercise restart recovery without a real DB.
 */
class IChaynsThreadLedger
{
  public:
    virtual ~IChaynsThreadLedger() = default;

    virtual bool isEnabled() const = 0;
    virtual std::optional<ThreadLedgerRow> loadThreadBySessionId(
        const std::string& sessionId,
        std::string* errorMessage = nullptr) = 0;

    virtual void asyncUpsertThread(const ThreadLedgerRow& row) = 0;
    virtual void asyncDetachThreadBySessionId(const std::string& sessionId) = 0;
    virtual void asyncUpdateThreadSessionId(const std::string& oldSessionId,
                                            const std::string& newSessionId) = 0;
};

}  // namespace chayns
