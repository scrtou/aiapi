#include <infrastructure/account/DrogonAccountHttpTransport.h>
#include <infrastructure/http/SynchronousHttpClient.h>

#include <drogon/drogon.h>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace account {
namespace {

drogon::HttpMethod toDrogonMethod(HttpMethod method)
{
    switch (method) {
        case HttpMethod::Get: return drogon::Get;
        case HttpMethod::Post: return drogon::Post;
        case HttpMethod::Delete: return drogon::Delete;
    }
    return drogon::Get;
}

bool isContentTypeHeader(std::string_view name)
{
    constexpr std::string_view kContentType = "content-type";
    return name.size() == kContentType.size() &&
           std::equal(name.begin(), name.end(), kContentType.begin(),
                      [](unsigned char left, unsigned char right) {
                          return std::tolower(left) == std::tolower(right);
                      });
}

class DrogonAccountHttpTransport final : public IAccountHttpTransport
{
  public:
    HttpResult send(const std::string& baseUrl,
                    const HttpRequest& request,
                    double timeoutSeconds) override
    {
        auto native = drogon::HttpRequest::newHttpRequest();
        native->setMethod(toDrogonMethod(request.method));
        native->setPath(request.path);
        for (const auto& [name, value] : request.headers) {
            // Drogon stores Content-Type separately from its generic header
            // map.  Adding it through addHeader() leaves that internal value
            // unset, so a request with a body gets Drogon's default content
            // type plus this one.  HTTP consumers such as FastAPI can then
            // parse the first value and reject an otherwise valid JSON body.
            if (isContentTypeHeader(name)) {
                native->setContentTypeString(value);
            } else {
                native->addHeader(name, value);
            }
        }
        native->setBody(request.body);

        auto client = infrastructure::http::makeSynchronousHttpClient(baseUrl);
        if (!client) {
            return {HttpResultCode::BadResponse, nullptr};
        }
        const auto [result, response] = client->sendRequest(native, timeoutSeconds);
        if (result != drogon::ReqResult::Ok || !response) {
            return {HttpResultCode::BadResponse, nullptr};
        }

        auto adapted = std::make_shared<HttpResponse>();
        adapted->statusCode = static_cast<int>(response->getStatusCode());
        adapted->body = response->getBody();
        adapted->headers.emplace("content-type", response->getHeader("content-type"));
        return {HttpResultCode::Ok, std::move(adapted)};
    }
};

}  // namespace

std::shared_ptr<IAccountHttpTransport> makeDrogonAccountHttpTransport()
{
    return std::make_shared<DrogonAccountHttpTransport>();
}

}  // namespace account
