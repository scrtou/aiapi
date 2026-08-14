#pragma once

#include <domain/port/IProviderRegistry.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace provider {

/**
 * Runtime-owned provider registry.
 *
 * Providers are registered synchronously while AppContext is being built and
 * the registry is frozen before it is published to request handlers.  Reads
 * therefore need no lock and cannot observe a partially populated registry.
 */
class ProviderRegistry final : public IProviderRegistry
{
  public:
    [[nodiscard]] bool registerProvider(
        std::string name,
        std::shared_ptr<APIinterface> provider);

    void freeze() noexcept { frozen_ = true; }
    [[nodiscard]] bool isFrozen() const noexcept { return frozen_; }

    [[nodiscard]] std::shared_ptr<APIinterface> findProvider(
        const std::string& apiName) const override;

    [[nodiscard]] std::vector<std::string> providerNames() const;

  private:
    std::unordered_map<std::string, std::shared_ptr<APIinterface>> providers_;
    bool frozen_ = false;
};

}  // namespace provider
