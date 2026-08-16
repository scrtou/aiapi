#include <infrastructure/provider/chayns/ChaynsThreadContext.h>

#include <platform/Log.h>

#include <ctime>
#include <unordered_set>
#include <utility>

namespace chayns {

ChaynsThreadContext::ChaynsThreadContext(
    std::shared_ptr<IChaynsThreadLedger> ledger)
    : m_ledger(std::move(ledger))
{
}

bool ChaynsThreadContext::isComplete(const ThreadContext& context)
{
    return !context.threadId.empty() && !context.accountUserName.empty() &&
           !context.modelId.empty() && !context.accountType.empty() &&
           !context.origin.empty() && !context.referer.empty() &&
           context.threadTypeId > 0 &&
           (context.accountType != "pro" || context.workspaceUacId > 0);
}

ThreadContext ChaynsThreadContext::fromLedger(const ThreadLedgerRow& row)
{
    ThreadContext context;
    context.threadId = row.threadId;
    context.userAuthorId = row.userAuthorId;
    context.agentAuthorId = row.agentAuthorId;
    context.accountUserName = row.accountUserName;
    context.modelId = row.modelId;
    context.accountType = row.accountType;
    context.threadTypeId = row.threadTypeId;
    context.workspaceUacId = row.workspaceUacId;
    context.origin = row.origin;
    context.referer = row.referer;
    context.lastRequestMessageId = row.lastRequestMessageId;
    context.lastRequestCreationTime = row.lastRequestCreationTime;
    context.lastAssistantMessageId = row.lastAssistantMessageId;
    return context;
}

ThreadLedgerRow ChaynsThreadContext::toLedger(
    const std::string& conversationId,
    const ThreadContext& context)
{
    ThreadLedgerRow row;
    row.threadId = context.threadId;
    row.sessionId = conversationId;
    row.userAuthorId = context.userAuthorId;
    row.agentAuthorId = context.agentAuthorId;
    row.accountUserName = context.accountUserName;
    row.modelId = context.modelId;
    row.accountType = context.accountType;
    row.threadTypeId = context.threadTypeId;
    row.workspaceUacId = context.workspaceUacId;
    row.origin = context.origin;
    row.referer = context.referer;
    row.lastRequestMessageId = context.lastRequestMessageId;
    row.lastRequestCreationTime = context.lastRequestCreationTime;
    row.lastAssistantMessageId = context.lastAssistantMessageId;
    row.createdAt = static_cast<std::int64_t>(std::time(nullptr));
    row.lastActiveAt = row.createdAt;
    return row;
}

ThreadContextLookup ChaynsThreadContext::lookup(
    const std::string& conversationId,
    const std::string& fallbackConversationId)
{
    ThreadContextLookup result;
    if (conversationId.empty()) return result;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_contexts.find(conversationId);
        if (it != m_contexts.end()) {
            result.context = it->second;
            return result;
        }
    }

    if (!m_ledger || !m_ledger->isEnabled()) return result;

    auto persisted = m_ledger->loadThreadBySessionId(conversationId);
    if (!persisted.has_value() && !fallbackConversationId.empty() &&
        fallbackConversationId != conversationId) {
        persisted = m_ledger->loadThreadBySessionId(fallbackConversationId);
        result.usedFallbackKey = persisted.has_value();
    }
    if (!persisted.has_value()) return result;

    auto restored = fromLedger(*persisted);
    if (!isComplete(restored)) {
        result.incompleteLedgerContext = *persisted;
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto [it, inserted] = m_contexts.emplace(conversationId, std::move(restored));
        result.context = it->second;
        LOG_INFO << "[chaynsAPI][ThreadContext] restored: provider=chaynsapi"
                 << ", conversationId=" << conversationId
                 << ", upstreamThreadId=" << it->second.threadId
                 << ", threadIdPresent="
                 << !it->second.threadId.empty()
                 << ", fallbackKeyUsed=" << result.usedFallbackKey
                 << ", inserted=" << inserted;
    }
    return result;
}

void ChaynsThreadContext::cache(const std::string& conversationId,
                                ThreadContext context)
{
    if (conversationId.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_contexts[conversationId] = std::move(context);
}

void ChaynsThreadContext::store(const std::string& conversationId,
                                const ThreadContext& context)
{
    cache(conversationId, context);
    if (m_ledger && m_ledger->isEnabled() && !context.threadId.empty()) {
        m_ledger->asyncUpsertThread(toLedger(conversationId, context));
    }
}

void ChaynsThreadContext::detach(const std::vector<std::string>& conversationIds)
{
    std::vector<std::string> ids;
    ids.reserve(conversationIds.size());
    std::unordered_set<std::string> uniqueIds;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& id : conversationIds) {
            if (id.empty() || !uniqueIds.insert(id).second) continue;
            m_contexts.erase(id);
            ids.push_back(id);
        }
    }
    if (!m_ledger || !m_ledger->isEnabled()) return;
    for (const auto& id : ids) m_ledger->asyncDetachThreadBySessionId(id);
}

platform::Result<void> ChaynsThreadContext::erase(
    const std::string& conversationId)
{
    if (conversationId.empty()) return platform::Result<void>::success();
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        erased = m_contexts.erase(conversationId) != 0;
    }
    LOG_INFO << "[chaynsAPI][ThreadContext] erase: provider=chaynsapi"
             << ", conversationId=" << conversationId
             << ", erased=" << erased;
    if (m_ledger && m_ledger->isEnabled()) {
        m_ledger->asyncDetachThreadBySessionId(conversationId);
    }
    return platform::Result<void>::success();
}

platform::Result<void> ChaynsThreadContext::transfer(
    const std::string& oldConversationId,
    const std::string& newConversationId)
{
    if (oldConversationId.empty() || newConversationId.empty() ||
        oldConversationId == newConversationId) {
        return platform::Result<void>::success();
    }
    bool transferred = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_contexts.find(oldConversationId);
        if (it != m_contexts.end()) {
            m_contexts[newConversationId] = it->second;
            m_contexts.erase(it);
            transferred = true;
        }
    }
    if (!transferred) {
        // A process restart clears the memory map, but the durable row may
        // still be the only copy when the application rotates its local
        // conversation key.  Rotate that row as well so the reaper does not
        // mistake an active upstream thread for an orphan.
        if (m_ledger && m_ledger->isEnabled()) {
            const auto persisted = m_ledger->loadThreadBySessionId(oldConversationId);
            if (persisted.has_value()) {
                m_ledger->asyncUpdateThreadSessionId(
                    oldConversationId, newConversationId);
                if (const auto restored = fromLedger(*persisted);
                    isComplete(restored)) {
                    cache(newConversationId, restored);
                }
                LOG_INFO << "[chaynsAPI][ThreadContext] transfer: provider=chaynsapi"
                         << ", conversationId=" << newConversationId
                         << ", previousConversationId=" << oldConversationId
                         << ", durableOnly=true";
                return platform::Result<void>::success();
            }
        }
        LOG_WARN << "[chaynsAPI][ThreadContext] transfer: provider=chaynsapi"
                 << ", conversationId=" << oldConversationId
                 << ", oldContextMissing=true";
        return platform::Result<void>::success();
    }
    if (m_ledger && m_ledger->isEnabled()) {
        m_ledger->asyncUpdateThreadSessionId(oldConversationId, newConversationId);
    }
    LOG_INFO << "[chaynsAPI][ThreadContext] transfer: provider=chaynsapi"
             << ", conversationId=" << newConversationId
             << ", previousConversationId=" << oldConversationId;
    return platform::Result<void>::success();
}

}  // namespace chayns
