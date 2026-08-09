#ifndef RETOOL_HTTP_TRANSPORT_H
#define RETOOL_HTTP_TRANSPORT_H

#include <drogon/drogon.h>

#include <memory>
#include <string>
#include <utility>

namespace retool {

using HttpResult = std::pair<drogon::ReqResult, drogon::HttpResponsePtr>;

/**
 * Narrow synchronous I/O seam for the Retool adapter.
 *
 * Retry, polling, workspace selection and request-shape policy deliberately
 * remain in retoolapi.  Tests replace only the socket-facing operation.
 */
class IRetoolHttpTransport
{
  public:
    virtual ~IRetoolHttpTransport() = default;
    virtual HttpResult send(const std::string& baseUrl,
                            const drogon::HttpRequestPtr& request,
                            double timeoutSeconds) = 0;
};

std::shared_ptr<IRetoolHttpTransport> makeDrogonRetoolHttpTransport();

}  // namespace retool

#endif  // RETOOL_HTTP_TRANSPORT_H
