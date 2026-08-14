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
 *
 * Each provider has one narrow chat registration plus optional catalog and
 * thread-context capabilities. There is intentionally no legacy lane.
 */
class ProviderRegistry final : public IProviderRegistry
{
  public:
    [[nodiscard]] bool registerChatProvider(
        std::string name,
        std::shared_ptr<IChatProvider> provider,
        std::shared_ptr<IProviderModelCatalog> catalog = nullptr,
        std::shared_ptr<IProviderThreadContext> threadContext = nullptr);

    void freeze() noexcept { frozen_ = true; }
    [[nodiscard]] bool isFrozen() const noexcept { return frozen_; }

    [[nodiscard]] std::shared_ptr<IChatProvider> findChatProvider(
        const std::string& apiName) const override;

    [[nodiscard]] std::shared_ptr<IProviderModelCatalog> findModelCatalog(
        const std::string& apiName) const override;

    [[nodiscard]] std::shared_ptr<IProviderThreadContext> findThreadContext(
        const std::string& apiName) const override;

    [[nodiscard]] std::vector<std::string> providerNames() const;

  private:
    std::unordered_map<std::string, std::shared_ptr<IChatProvider>> chatProviders_;
    std::unordered_map<std::string, std::shared_ptr<IProviderModelCatalog>> catalogs_;
    std::unordered_map<std::string, std::shared_ptr<IProviderThreadContext>> threadContexts_;
    bool frozen_ = false;
};

}  // namespace provider
