#pragma once

#include "IManagedAccountBackend.h"

class ClassicProviderAccountBackend : public IManagedAccountBackend
{
  public:
    ManagedAccountKind kind() const override { return ManagedAccountKind::ClassicProviderAccount; }
    std::vector<ManagedAccountRecord> list() override;
    std::optional<ManagedAccountRecord> get(const std::string& id) override;
    bool disable(const std::string& id, std::string* errorMessage = nullptr) override;
    std::optional<ManagedExecutionContext> buildExecutionContext(const std::string& id,
                                                                 std::string* errorMessage = nullptr) override;
};
