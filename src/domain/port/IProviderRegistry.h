#ifndef DOMAIN_PORT_IPROVIDER_REGISTRY_H
#define DOMAIN_PORT_IPROVIDER_REGISTRY_H

#include <domain/port/IChatProvider.h>
#include <domain/port/IProviderModelCatalog.h>
#include <domain/port/IProviderThreadContext.h>

#include <memory>
#include <string>

/**
 * Runtime provider lookup port.
 *
 * All runtime providers are exposed through narrow capabilities. A lookup
 * only returns the capability it names; there is no compatibility lane that
 * could reintroduce a session-mutating provider implementation.
 */
class IProviderRegistry
{
  public:
    virtual ~IProviderRegistry() = default;

    virtual std::shared_ptr<provider::IChatProvider> findChatProvider(
        const std::string&) const = 0;

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
