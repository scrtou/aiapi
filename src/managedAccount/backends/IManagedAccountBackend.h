#pragma once

#include <managedAccount/contracts/ManagedAccount.h>
#include <optional>
#include <string>
#include <vector>

class IManagedAccountBackend
{
  public:
    virtual ~IManagedAccountBackend() = default;

    virtual ManagedAccountKind kind() const = 0;
    virtual std::vector<ManagedAccountRecord> list() = 0;
    virtual std::optional<ManagedAccountRecord> get(const std::string& id) = 0;
    virtual bool disable(const std::string& id, std::string* errorMessage = nullptr) = 0;
    virtual std::optional<ManagedExecutionContext> buildExecutionContext(const std::string& id,
                                                                         std::string* errorMessage = nullptr) = 0;
};
