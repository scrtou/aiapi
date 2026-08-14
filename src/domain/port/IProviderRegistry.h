#ifndef DOMAIN_PORT_IPROVIDER_REGISTRY_H
#define DOMAIN_PORT_IPROVIDER_REGISTRY_H

#include <domain/port/APIinterface.h>

#include <memory>
#include <string>

/**
 * Minimal provider lookup port for application/infrastructure consumers.
 *
 * Consumers do not know whether providers came from runtime configuration or
 * a test registry.
 */
class IProviderRegistry
{
  public:
    virtual ~IProviderRegistry() = default;
    virtual std::shared_ptr<APIinterface> findProvider(const std::string& apiName) const = 0;
};

#endif  // DOMAIN_PORT_IPROVIDER_REGISTRY_H
