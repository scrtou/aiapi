#pragma once

#include <domain/model/ProviderModelCatalog.h>

namespace provider {

/**
 * Read-only model catalog capability, deliberately separate from chat
 * generation.  A provider can expose a catalog without reopening the old
 * multi-purpose APIinterface port.
 */
class IProviderModelCatalog
{
  public:
    virtual ~IProviderModelCatalog() = default;
    virtual ProviderModelCatalog getModels() = 0;
};

}  // namespace provider
