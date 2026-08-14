#pragma once

#include <managedAccount/backends/IManagedAccountBackend.h>
#include <managedAccount/contracts/ManagedAccount.h>
#include <optional>
#include <memory>
#include <string>
#include <vector>

class ManagedAccountService final : public IManagedAccountContextResolver
{
  public:
    ManagedAccountService(std::shared_ptr<IManagedAccountBackend> classicBackend,
                          std::shared_ptr<IManagedAccountBackend> retoolBackend);

    std::vector<ManagedAccountRecord> listAll();
    std::vector<ManagedAccountRecord> listByKind(ManagedAccountKind kind);
    std::optional<ManagedAccountRecord> get(ManagedAccountKind kind, const std::string& id);
    bool disable(ManagedAccountKind kind, const std::string& id, std::string* errorMessage = nullptr);
    std::optional<ManagedExecutionContext> buildExecutionContext(ManagedAccountKind kind,
                                                                 const std::string& id,
                                                                 std::string* errorMessage = nullptr) override;

  private:
    std::shared_ptr<IManagedAccountBackend> classicBackend_;
    std::shared_ptr<IManagedAccountBackend> retoolBackend_;
};
