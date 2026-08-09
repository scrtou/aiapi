#include <apipoint/chaynsapi/ChaynsHttpTransport.h>

namespace chayns {
namespace {

class DrogonChaynsHttpTransport final : public IChaynsHttpTransport
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

std::shared_ptr<IChaynsHttpTransport> makeDrogonChaynsHttpTransport()
{
    return std::make_shared<DrogonChaynsHttpTransport>();
}

}  // namespace chayns
