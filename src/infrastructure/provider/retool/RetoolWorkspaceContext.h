#pragma once

#include <domain/port/IRetoolWorkspaceUseCase.h>
#include <platform/result/Result.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace retool {

/** Owns Retool conversation affinity, agent thread IDs, and usage leases. */
class RetoolWorkspaceContext final
{
  public:
    class UsageLease final
    {
      public:
        UsageLease() = default;
        ~UsageLease();
        UsageLease(const UsageLease&) = delete;
        UsageLease& operator=(const UsageLease&) = delete;
        UsageLease(UsageLease&& other) noexcept;
        UsageLease& operator=(UsageLease&& other) noexcept;

      private:
        UsageLease(workspace::IRetoolWorkspaceUseCase* workspaces,
                   std::string workspaceId,
                   bool active);
        void release() noexcept;

        workspace::IRetoolWorkspaceUseCase* m_workspaces = nullptr;
        std::string m_workspaceId;
        bool m_active = false;

        friend class RetoolWorkspaceContext;
    };

    explicit RetoolWorkspaceContext(
        workspace::IRetoolWorkspaceUseCase& workspaces);

    [[nodiscard]] std::string workspaceFor(
        const std::string& conversationId) const;
    void bindWorkspace(const std::string& conversationId,
                       const std::string& workspaceId);

    [[nodiscard]] std::string agentThreadFor(
        const std::string& conversationId) const;
    void bindAgentThread(const std::string& conversationId,
                         const std::string& threadId);
    void eraseAgentThread(const std::string& conversationId);

    [[nodiscard]] UsageLease startUsage(const std::string& workspaceId);

    platform::Result<void> erase(const std::string& conversationId);
    platform::Result<void> transfer(const std::string& oldConversationId,
                                    const std::string& newConversationId);

  private:
    workspace::IRetoolWorkspaceUseCase* m_workspaces;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::string> m_agentThreads;
    std::unordered_map<std::string, std::string> m_workspaceAffinity;
};

}  // namespace retool
