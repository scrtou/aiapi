#include <infrastructure/provider/retool/RetoolWorkspaceContext.h>

#include <utility>

namespace retool {

RetoolWorkspaceContext::UsageLease::UsageLease(
    workspace::IRetoolWorkspaceUseCase* workspaces,
    std::string workspaceId,
    bool active)
    : m_workspaces(workspaces),
      m_workspaceId(std::move(workspaceId)),
      m_active(active)
{
}

RetoolWorkspaceContext::UsageLease::~UsageLease()
{
    release();
}

RetoolWorkspaceContext::UsageLease::UsageLease(UsageLease&& other) noexcept
    : m_workspaces(other.m_workspaces),
      m_workspaceId(std::move(other.m_workspaceId)),
      m_active(other.m_active)
{
    other.m_workspaces = nullptr;
    other.m_active = false;
}

RetoolWorkspaceContext::UsageLease&
RetoolWorkspaceContext::UsageLease::operator=(UsageLease&& other) noexcept
{
    if (this == &other) return *this;
    release();
    m_workspaces = other.m_workspaces;
    m_workspaceId = std::move(other.m_workspaceId);
    m_active = other.m_active;
    other.m_workspaces = nullptr;
    other.m_active = false;
    return *this;
}

void RetoolWorkspaceContext::UsageLease::release() noexcept
{
    if (m_active && m_workspaces && !m_workspaceId.empty()) {
        m_workspaces->markUsageFinished(m_workspaceId, nullptr);
    }
    m_active = false;
}

RetoolWorkspaceContext::RetoolWorkspaceContext(
    workspace::IRetoolWorkspaceUseCase& workspaces)
    : m_workspaces(&workspaces)
{
}

std::string RetoolWorkspaceContext::workspaceFor(
    const std::string& conversationId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_workspaceAffinity.find(conversationId);
    return it == m_workspaceAffinity.end() ? std::string{} : it->second;
}

void RetoolWorkspaceContext::bindWorkspace(
    const std::string& conversationId,
    const std::string& workspaceId)
{
    if (conversationId.empty() || workspaceId.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_workspaceAffinity[conversationId] = workspaceId;
}

std::string RetoolWorkspaceContext::agentThreadFor(
    const std::string& conversationId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_agentThreads.find(conversationId);
    return it == m_agentThreads.end() ? std::string{} : it->second;
}

void RetoolWorkspaceContext::bindAgentThread(
    const std::string& conversationId,
    const std::string& threadId)
{
    if (conversationId.empty() || threadId.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_agentThreads[conversationId] = threadId;
}

void RetoolWorkspaceContext::eraseAgentThread(
    const std::string& conversationId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_agentThreads.erase(conversationId);
}

RetoolWorkspaceContext::UsageLease RetoolWorkspaceContext::startUsage(
    const std::string& workspaceId)
{
    const bool active = m_workspaces && !workspaceId.empty() &&
        m_workspaces->markUsageStarted(workspaceId, nullptr);
    return UsageLease(m_workspaces, workspaceId, active);
}

platform::Result<void> RetoolWorkspaceContext::erase(
    const std::string& conversationId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_agentThreads.erase(conversationId);
    m_workspaceAffinity.erase(conversationId);
    return platform::Result<void>::success();
}

platform::Result<void> RetoolWorkspaceContext::transfer(
    const std::string& oldConversationId,
    const std::string& newConversationId)
{
    if (oldConversationId.empty() || newConversationId.empty() ||
        oldConversationId == newConversationId) {
        return platform::Result<void>::success();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto thread = m_agentThreads.find(oldConversationId);
    if (thread != m_agentThreads.end()) {
        m_agentThreads[newConversationId] = thread->second;
        m_agentThreads.erase(thread);
    }
    const auto workspace = m_workspaceAffinity.find(oldConversationId);
    if (workspace != m_workspaceAffinity.end()) {
        m_workspaceAffinity[newConversationId] = workspace->second;
        m_workspaceAffinity.erase(workspace);
    }
    return platform::Result<void>::success();
}

}  // namespace retool
