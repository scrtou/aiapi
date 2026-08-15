#include <drogon/drogon.h>

#include <infrastructure/account/DrogonAccountHttpTransport.h>
#include <infrastructure/provider/chayns/ChaynsHttpTransport.h>
#include <infrastructure/provider/retool/RetoolHttpTransport.h>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

class LoopbackHttpServer
{
  public:
    explicit LoopbackHttpServer(int expectedRequests)
        : expectedRequests_(expectedRequests)
    {
    }

    ~LoopbackHttpServer()
    {
        const int fd = listenFd_.exchange(-1);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool start()
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            std::cerr << "socket failed: " << std::strerror(errno) << std::endl;
            return false;
        }

        int reuse = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(fd, expectedRequests_) != 0) {
            std::cerr << "bind/listen failed: " << std::strerror(errno) << std::endl;
            ::close(fd);
            return false;
        }

        socklen_t length = sizeof(address);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            std::cerr << "getsockname failed: " << std::strerror(errno) << std::endl;
            ::close(fd);
            return false;
        }

        port_ = ntohs(address.sin_port);
        listenFd_.store(fd);
        worker_ = std::thread([this, fd] { serve(fd); });
        return true;
    }

    std::uint16_t port() const { return port_; }

  private:
    void serve(int fd) const
    {
        static constexpr char kResponse[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 2\r\n"
            "Connection: close\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "ok";

        for (int served = 0; served < expectedRequests_; ++served) {
            const int client = ::accept(fd, nullptr, nullptr);
            if (client < 0) {
                return;
            }

            std::size_t sent = 0;
            while (sent < sizeof(kResponse) - 1) {
                const auto wrote = ::send(client,
                                          kResponse + sent,
                                          sizeof(kResponse) - 1 - sent,
                                          MSG_NOSIGNAL);
                if (wrote <= 0) {
                    break;
                }
                sent += static_cast<std::size_t>(wrote);
            }
            ::shutdown(client, SHUT_RDWR);
            ::close(client);
        }
    }

    int expectedRequests_;
    std::atomic<int> listenFd_{-1};
    std::thread worker_;
    std::uint16_t port_ = 0;
};

bool isOk(const std::pair<drogon::ReqResult, drogon::HttpResponsePtr>& result)
{
    return result.first == drogon::ReqResult::Ok && result.second &&
           result.second->statusCode() == drogon::k200OK;
}

drogon::HttpRequestPtr makeRequest()
{
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath("/");
    return request;
}

}  // namespace

// Isolated regression fixture for synchronous upstream transports.  Beginning
// advice is executed by Drogon's main event loop.  Every production transport
// below must therefore bind its synchronous HttpClient to a different running
// I/O loop; using newHttpClient(baseUrl) without a loop aborts here with
// Drogon's deadlock assertion.
int main()
{
    LoopbackHttpServer server(/*expectedRequests=*/3);
    if (!server.start()) {
        return 2;
    }

    Json::Value config(Json::objectValue);
    config["app"]["number_of_threads"] = 1;
    config["app"]["handle_sig_term"] = false;
    config["app"]["upload_path"] = "/tmp/aiapi-startup-sync-http-fixture";

    bool transportsReady = false;
    try {
        drogon::app().loadConfigJson(config);
        drogon::app().registerBeginningAdvice([&server, &transportsReady]() {
            const std::string baseUrl =
                "http://127.0.0.1:" + std::to_string(server.port());

            account::HttpRequest accountRequest;
            accountRequest.method = account::HttpMethod::Get;
            accountRequest.path = "/";
            const auto accountResult = account::makeDrogonAccountHttpTransport()->send(
                baseUrl, accountRequest, 2.0);
            const bool accountOk = accountResult.first == account::HttpResultCode::Ok &&
                                   accountResult.second &&
                                   accountResult.second->statusCode == 200;

            const bool chaynsOk = isOk(
                chayns::makeDrogonChaynsHttpTransport()->send(baseUrl, makeRequest(), 2.0));
            const bool retoolOk = isOk(
                retool::makeDrogonRetoolHttpTransport()->send(baseUrl, makeRequest(), 2.0));

            transportsReady = accountOk && chaynsOk && retoolOk;
            std::cout << (transportsReady ? "SYNC_HTTP_TRANSPORTS_READY"
                                          : "SYNC_HTTP_TRANSPORTS_FAILED")
                      << std::endl;
            drogon::app().quit();
        });
        drogon::app().run();
    } catch (const std::exception& ex) {
        std::cerr << "fixture exception: " << ex.what() << std::endl;
        return 3;
    }
    return transportsReady ? 0 : 1;
}
