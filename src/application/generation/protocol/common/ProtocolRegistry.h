#pragma once

#include <application/generation/protocol/common/ProtocolContracts.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>

namespace generation::protocol {

/** Registry for protocol modules and their externally visible routes. */
class ProtocolRegistry {
  public:
    bool registerModule(std::shared_ptr<IProtocolModule> module);
    bool registerRoute(std::string method,
                       std::string path,
                       std::string protocolId,
                       std::string operation);

    const IProtocolModule* find(const std::string& protocolId) const;
    const IProtocolModule* findRoute(const std::string& method,
                                     const std::string& path) const;
    std::string findRouteOperation(const std::string& method,
                                   const std::string& path) const;
    ProtocolDispatchResult dispatch(const RawProtocolRequest& raw) const;

    bool validate(std::string* error = nullptr) const;
    const std::string& lastError() const noexcept { return lastError_; }

  private:
    struct Route {
        std::string protocolId;
        std::string operation;
    };

    static std::string normalizeMethod(std::string method);
    static std::string routeKey(const std::string& method, const std::string& path);

    std::unordered_map<std::string, std::shared_ptr<IProtocolModule>> modules_;
    std::map<std::string, Route> routes_;
    std::string lastError_;
};

/** Build the production registry for the currently supported routes. */
std::shared_ptr<ProtocolRegistry> makeDefaultProtocolRegistry();

}  // namespace generation::protocol
