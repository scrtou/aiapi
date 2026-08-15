#include <infrastructure/provider/chayns/ChaynsHttpTransport.h>
#include <infrastructure/http/SynchronousHttpClient.h>

namespace chayns {
namespace {

class DrogonChaynsHttpTransport final : public IChaynsHttpTransport
{
  public:
    HttpResult send(const std::string& baseUrl,
                    const drogon::HttpRequestPtr& request,
                    double timeoutSeconds) override
    {
        auto client = infrastructure::http::makeSynchronousHttpClient(baseUrl);
        if (!client) {
            return {drogon::ReqResult::BadResponse, nullptr};
        }
        return client->sendRequest(request, timeoutSeconds);
    }
};

}  // namespace

std::shared_ptr<IChaynsHttpTransport> makeDrogonChaynsHttpTransport()
{
    return std::make_shared<DrogonChaynsHttpTransport>();
}

}  // namespace chayns
