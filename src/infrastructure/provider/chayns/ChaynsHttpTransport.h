#ifndef CHAYNS_HTTP_TRANSPORT_H
#define CHAYNS_HTTP_TRANSPORT_H

#include <drogon/drogon.h>

#include <memory>
#include <string>
#include <utility>

namespace chayns {

using HttpResult = std::pair<drogon::ReqResult, drogon::HttpResponsePtr>;

/**
 * Narrow synchronous transport seam for the Chayns adapter.
 *
 * It owns no retry, polling, account or wire-format policy.  Those behaviours
 * remain in chaynsapi; tests can replace only the external I/O boundary.
 */
class IChaynsHttpTransport
{
  public:
    virtual ~IChaynsHttpTransport() = default;
    virtual HttpResult send(const std::string& baseUrl,
                            const drogon::HttpRequestPtr& request,
                            double timeoutSeconds) = 0;
};

std::shared_ptr<IChaynsHttpTransport> makeDrogonChaynsHttpTransport();

}  // namespace chayns

#endif  // CHAYNS_HTTP_TRANSPORT_H
