#include <accountManager/AccountHttpTransport.h>

namespace account {
namespace {

class DrogonAccountHttpTransport final : public IAccountHttpTransport
{
  public:
    HttpResult send(const std::string& baseUrl,
                    const drogon::HttpRequestPtr& request,
                    double timeoutSeconds) override
    {
        auto client = drogon::HttpClient::newHttpClient(baseUrl);
        return client->sendRequest(request, timeoutSeconds);
    }
};

}  // namespace

std::shared_ptr<IAccountHttpTransport> makeDrogonAccountHttpTransport()
{
    return std::make_shared<DrogonAccountHttpTransport>();
}

}  // namespace account
