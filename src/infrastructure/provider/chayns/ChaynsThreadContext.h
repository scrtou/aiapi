#pragma once

#include <domain/port/IChaynsThreadLedger.h>
#include <platform/result/Result.h>

#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace chayns {

/**
 * Provider-owned state required to continue one Chayns upstream thread.
 *
 * This is deliberately a credential-free value.  Account tokens and person
 * identifiers continue to belong to the account selector; this context only
 * records the routing and message anchors needed by the protocol adapter.
 */
struct ThreadContext
{
    std::string threadId;
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
};

/** Result of resolving a previous local conversation key. */
struct ThreadContextLookup
{
    std::optional<ThreadContext> context;
    // Rows written by an older schema may not contain enough routing data to
    // restore safely.  The orchestrator may upgrade such a row only after it
    // has validated the current model/account route.
    std::optional<ThreadLedgerRow> incompleteLedgerContext;
    bool usedFallbackKey = false;
};

/**
 * Owns the process-local conversation map and its narrow durable ledger seam.
 * No HTTP or application/session types cross this boundary.
 */
class ChaynsThreadContext final
{
  public:
    explicit ChaynsThreadContext(
        std::shared_ptr<IChaynsThreadLedger> ledger = nullptr);

    [[nodiscard]] ThreadContextLookup lookup(
        const std::string& conversationId,
        const std::string& fallbackConversationId = {});

    /** Replace/cache a context without performing durable I/O. */
    void cache(const std::string& conversationId, ThreadContext context);

    /** Cache and asynchronously persist a complete provider context. */
    void store(const std::string& conversationId,
               const ThreadContext& context);

    /** Remove local mappings and detach their durable ledger rows. */
    void detach(const std::vector<std::string>& conversationIds);

    platform::Result<void> erase(const std::string& conversationId);
    platform::Result<void> transfer(const std::string& oldConversationId,
                                    const std::string& newConversationId);

  private:
    static bool isComplete(const ThreadContext& context);
    static ThreadContext fromLedger(const ThreadLedgerRow& row);
    static ThreadLedgerRow toLedger(const std::string& conversationId,
                                    const ThreadContext& context);

    std::shared_ptr<IChaynsThreadLedger> m_ledger;
    std::map<std::string, ThreadContext> m_contexts;
    std::mutex m_mutex;
};

}  // namespace chayns
