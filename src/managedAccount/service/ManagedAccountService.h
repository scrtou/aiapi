#pragma once

#include <managedAccount/backends/ClassicProviderAccountBackend.h>
#include <managedAccount/backends/RetoolWorkspaceBackend.h>
#include <managedAccount/contracts/ManagedAccount.h>
#include <optional>
#include <string>
#include <vector>

class ManagedAccountService
{
  public:
    static ManagedAccountService& getInstance()
    {
        static ManagedAccountService instance;
        return instance;
    }

    std::vector<ManagedAccountRecord> listAll();
    std::vector<ManagedAccountRecord> listByKind(ManagedAccountKind kind);
    std::optional<ManagedAccountRecord> get(ManagedAccountKind kind, const std::string& id);
    bool disable(ManagedAccountKind kind, const std::string& id, std::string* errorMessage = nullptr);
    std::optional<ManagedExecutionContext> buildExecutionContext(ManagedAccountKind kind,
                                                                 const std::string& id,
                                                                 std::string* errorMessage = nullptr);

  private:
    ManagedAccountService() = default;

    ClassicProviderAccountBackend classicBackend_;
    RetoolWorkspaceBackend retoolBackend_;
};
