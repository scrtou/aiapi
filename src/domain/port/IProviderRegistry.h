#ifndef DOMAIN_PORT_IPROVIDER_REGISTRY_H
#define DOMAIN_PORT_IPROVIDER_REGISTRY_H

#include <domain/port/APIinterface.h>
#include <domain/port/IChatProvider.h>
#include <domain/port/IProviderModelCatalog.h>
#include <domain/port/IProviderThreadContext.h>

#include <memory>
#include <string>

/**
 * Runtime provider lookup port.
 *
 * `findProvider()` is the temporary wide legacy lane retained only while the
 * Retool slice is still on APIinterface.  New P6 slices resolve their narrow
 * capabilities directly; the default null implementations let old test
 * registries model only the legacy lane during the transition.
 */
class IProviderRegistry
{
  public:
    virtual ~IProviderRegistry() = default;

    virtual std::shared_ptr<APIinterface> findProvider(const std::string& apiName) const = 0;

    virtual std::shared_ptr<provider::IChatProvider> findChatProvider(
        const std::string&) const
    {
        return nullptr;
    }

    virtual std::shared_ptr<provider::IProviderModelCatalog> findModelCatalog(
        const std::string&) const
    {
        return nullptr;
    }

    virtual std::shared_ptr<provider::IProviderThreadContext> findThreadContext(
        const std::string&) const
    {
        return nullptr;
    }
};

#endif  // DOMAIN_PORT_IPROVIDER_REGISTRY_H
