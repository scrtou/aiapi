#include <infrastructure/account/DrogonAccountHttpTransport.h>

#include <drogon/drogon.h>

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
            native->addHeader(name, value);
        }
        native->setBody(request.body);

        auto client = drogon::HttpClient::newHttpClient(baseUrl);
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
