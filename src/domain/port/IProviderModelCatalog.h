#pragma once

#include <domain/model/ProviderModelCatalog.h>

namespace provider {

/**
 * Read-only model catalog capability, deliberately separate from chat
 * generation. A provider can expose a catalog without reopening a
 * multi-purpose provider interface.
 */
class IProviderModelCatalog
{
  public:
    virtual ~IProviderModelCatalog() = default;
    virtual ProviderModelCatalog getModels() = 0;

    /**
     * Non-blocking lookup for capabilities already present in the provider's
     * model cache. Implementations must not refresh remote state here because
     * generation admission may run on a transport event loop.
     */
    virtual std::optional<ProviderModelCapabilities> findModelCapabilities(
        const std::string&) const
    {
        return std::nullopt;
    }
};

}  // namespace provider
