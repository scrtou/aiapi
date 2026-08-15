#include <infrastructure/provider/retool/RetoolHttpTransport.h>
#include <infrastructure/http/SynchronousHttpClient.h>

namespace retool {
namespace {

class DrogonRetoolHttpTransport final : public IRetoolHttpTransport
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

std::shared_ptr<IRetoolHttpTransport> makeDrogonRetoolHttpTransport()
{
    return std::make_shared<DrogonRetoolHttpTransport>();
}

}  // namespace retool
