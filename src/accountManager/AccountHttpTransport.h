#ifndef ACCOUNT_HTTP_TRANSPORT_H
#define ACCOUNT_HTTP_TRANSPORT_H

#include <drogon/drogon.h>

#include <memory>
#include <string>
#include <utility>

namespace account {

using HttpResult = std::pair<drogon::ReqResult, drogon::HttpResponsePtr>;

/** Narrow account lifecycle HTTP seam; policy remains in AccountManager. */
class IAccountHttpTransport
{
  public:
    virtual ~IAccountHttpTransport() = default;
    virtual HttpResult send(const std::string& baseUrl,
                            const drogon::HttpRequestPtr& request,
                            double timeoutSeconds) = 0;
};

std::shared_ptr<IAccountHttpTransport> makeDrogonAccountHttpTransport();

}  // namespace account

#endif  // ACCOUNT_HTTP_TRANSPORT_H
