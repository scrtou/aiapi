#include <apipoint/retoolapi/RetoolHttpTransport.h>

namespace retool {
namespace {

class DrogonRetoolHttpTransport final : public IRetoolHttpTransport
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

std::shared_ptr<IRetoolHttpTransport> makeDrogonRetoolHttpTransport()
{
    return std::make_shared<DrogonRetoolHttpTransport>();
}

}  // namespace retool
