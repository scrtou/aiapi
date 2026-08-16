#include <infrastructure/provider/chayns/ChaynsPollingLoop.h>

#include <infrastructure/provider/chayns/ChaynsPollingPolicy.h>
#include <platform/Log.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace chayns {

ChaynsPollingLoop::ChaynsPollingLoop(
    std::shared_ptr<ChaynsProtocolClient> protocolClient,
    std::shared_ptr<IChaynsClock> clock)
    : m_protocolClient(std::move(protocolClient)), m_clock(std::move(clock))
{
    if (!m_protocolClient || !m_clock) {
        throw std::invalid_argument("ChaynsPollingLoop requires protocol client and clock");
    }
}

platform::Result<PollingResult> ChaynsPollingLoop::poll(
    MessageAnchor anchor,
    const Accountinfo_st& account,
    const policy::RequestRoute& route,
    std::chrono::steady_clock::time_point requestDeadline,
    provider::ProviderCallContext& context,
    std::string_view requestId,
    std::string_view conversationId) const
{
    PollingResult output;
    output.agentAuthorId = anchor.agentAuthorId;
    std::unordered_set<std::string> consumedMessageIds;
    const auto startedAt = m_clock->now();
    LOG_INFO << "[chaynsAPI][Polling] started: provider=chaynsapi"
             << ", requestId=" << requestId
             << ", conversationId=" << conversationId
             << ", upstreamThreadId=" << anchor.threadId
             << ", deadlineSeconds="
             << std::chrono::duration_cast<std::chrono::seconds>(
                    kRequestPollingDeadline).count();

    while (m_clock->now() < requestDeadline) {
        if (context.isCancelled()) {
            return platform::Result<PollingResult>::failure(
                platform::Error::cancelled("Chayns provider request cancelled"));
        }
        if (context.deadlineExceeded()) {
            return platform::Result<PollingResult>::failure(
                platform::Error::timeout("Chayns provider request deadline exceeded"));
        }

        ++output.pollCount;
        const auto nowBeforeSend = m_clock->now();
        const auto pollingRemaining = requestDeadline > nowBeforeSend
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                  requestDeadline - nowBeforeSend)
            : std::chrono::milliseconds::zero();
        const auto remaining = std::min(context.remaining(), pollingRemaining);
        const double remainingSeconds =
            static_cast<double>(remaining.count()) / 1000.0;
        std::optional<Json::Value> messages;
        if (remainingSeconds > 0.0 && !context.isCancelled()) {
            messages = m_protocolClient->getThreadMessages(
                anchor.threadId, anchor.creationTime, account, route,
                std::min(kUpstreamRequestTimeoutSeconds, remainingSeconds),
                requestId, conversationId);
        }

        if (context.isCancelled()) {
            return platform::Result<PollingResult>::failure(
                platform::Error::cancelled("Chayns provider request cancelled"));
        }
        if (context.deadlineExceeded()) {
            return platform::Result<PollingResult>::failure(
                platform::Error::timeout("Chayns provider request deadline exceeded"));
        }

        if (messages.has_value() && !messages->empty()) {
            const auto correlated =
                correlateMessageBatch(*messages, anchor, consumedMessageIds);
            if (anchor.agentAuthorId.empty() &&
                !correlated.inferredAgentAuthorId.empty()) {
                anchor.agentAuthorId = correlated.inferredAgentAuthorId;
                output.agentAuthorId = anchor.agentAuthorId;
            }
            for (const auto& reasoning : correlated.reasoningMessages) {
                output.reasoningMessages.append(reasoning);
            }
            if (correlated.status == CorrelationStatus::Superseded) {
                output.statusCode = 409;
                output.correlationConflict = true;
                output.found = true;
                break;
            }
            if (correlated.status == CorrelationStatus::FinalFound) {
                output.responseMessage =
                    correlated.finalMessage.get("text", "").asString();
                output.assistantMessageId =
                    correlated.finalMessage.get("id", "").asString();
                output.statusCode = 200;
                output.found = true;
                break;
            }
        }

        const auto now = m_clock->now();
        if (now >= requestDeadline) break;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - startedAt);
        const auto pollingSleepRemaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
            requestDeadline - now);
        const auto sleepRemaining = std::min(context.remaining(), pollingSleepRemaining);
        if (sleepRemaining <= std::chrono::milliseconds::zero()) break;
        m_clock->sleepFor(
            std::min(pollingDelayForElapsed(elapsed), sleepRemaining),
            [&context] {
                return context.isCancelled() || context.deadlineExceeded();
            });
    }

    LOG_INFO << "[chaynsAPI][Polling] finished: provider=chaynsapi"
             << ", requestId=" << requestId
             << ", conversationId=" << conversationId
             << ", upstreamThreadId=" << anchor.threadId
             << ", pollCount=" << output.pollCount
             << ", found=" << output.found
             << ", status=" << output.statusCode
             << ", reasoningCount=" << output.reasoningMessages.size();
    return platform::Result<PollingResult>::success(std::move(output));
}

}  // namespace chayns
