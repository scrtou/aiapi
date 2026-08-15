#pragma once

#include <domain/port/IRetoolWorkspaceUseCase.h>
#include <infrastructure/managedAccount/backends/IManagedAccountBackend.h>

class RetoolWorkspaceBackend : public IManagedAccountBackend
{
  public:
    explicit RetoolWorkspaceBackend(workspace::IRetoolWorkspaceUseCase& workspaces);

    ManagedAccountKind kind() const override { return ManagedAccountKind::RetoolWorkspace; }
    std::vector<ManagedAccountRecord> list() override;
    std::optional<ManagedAccountRecord> get(const std::string& id) override;
    bool disable(const std::string& id, std::string* errorMessage = nullptr) override;
    std::optional<ManagedExecutionContext> buildExecutionContext(const std::string& id,
                                                                 std::string* errorMessage = nullptr) override;

  private:
    workspace::IRetoolWorkspaceUseCase* workspaces_;
};
