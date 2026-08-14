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
 * During P6-W2/P6-W3 the registry has one narrow lane for migrated providers
 * and one legacy lane for the sole provider not yet sliced.  A provider name
 * may occupy exactly one lane, preventing a migrated provider from silently
 * falling back to APIinterface.
 */
class ProviderRegistry final : public IProviderRegistry
{
  public:
    [[nodiscard]] bool registerProvider(
        std::string name,
        std::shared_ptr<APIinterface> provider);

    [[nodiscard]] bool registerChatProvider(
        std::string name,
        std::shared_ptr<IChatProvider> provider,
        std::shared_ptr<IProviderModelCatalog> catalog = nullptr,
        std::shared_ptr<IProviderThreadContext> threadContext = nullptr);

    void freeze() noexcept { frozen_ = true; }
    [[nodiscard]] bool isFrozen() const noexcept { return frozen_; }

    [[nodiscard]] std::shared_ptr<APIinterface> findProvider(
        const std::string& apiName) const override;

    [[nodiscard]] std::shared_ptr<IChatProvider> findChatProvider(
        const std::string& apiName) const override;

    [[nodiscard]] std::shared_ptr<IProviderModelCatalog> findModelCatalog(
        const std::string& apiName) const override;

    [[nodiscard]] std::shared_ptr<IProviderThreadContext> findThreadContext(
        const std::string& apiName) const override;

    [[nodiscard]] std::vector<std::string> providerNames() const;

  private:
    [[nodiscard]] bool nameIsRegistered(const std::string& name) const;

    std::unordered_map<std::string, std::shared_ptr<APIinterface>> legacyProviders_;
    std::unordered_map<std::string, std::shared_ptr<IChatProvider>> chatProviders_;
    std::unordered_map<std::string, std::shared_ptr<IProviderModelCatalog>> catalogs_;
    std::unordered_map<std::string, std::shared_ptr<IProviderThreadContext>> threadContexts_;
    bool frozen_ = false;
};

}  // namespace provider
