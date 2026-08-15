#ifndef ACCOUNT_HTTP_TRANSPORT_H
#define ACCOUNT_HTTP_TRANSPORT_H

#include <map>
#include <memory>
#include <string>
#include <utility>

namespace account {

enum class HttpMethod {
    Get,
    Post,
    Delete,
};

enum class HttpResultCode {
    Ok,
    BadResponse,
};

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int statusCode = 0;
    std::string body;
    std::map<std::string, std::string> headers;

    std::string header(const std::string& name) const
    {
        const auto it = headers.find(name);
        return it == headers.end() ? std::string{} : it->second;
    }
};

using HttpResponsePtr = std::shared_ptr<HttpResponse>;
using HttpResult = std::pair<HttpResultCode, HttpResponsePtr>;

/** Narrow account lifecycle HTTP seam; policy remains in AccountManager. */
class IAccountHttpTransport
{
  public:
    virtual ~IAccountHttpTransport() = default;
    virtual HttpResult send(const std::string& baseUrl,
                            const HttpRequest& request,
                            double timeoutSeconds) = 0;
};

}  // namespace account

#endif  // ACCOUNT_HTTP_TRANSPORT_H
