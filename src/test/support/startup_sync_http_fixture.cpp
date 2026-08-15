#include <drogon/drogon.h>

#include <infrastructure/account/DrogonAccountHttpTransport.h>
#include <infrastructure/provider/chayns/ChaynsHttpTransport.h>
#include <infrastructure/provider/retool/RetoolHttpTransport.h>

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

bool equalsIgnoreCase(std::string_view left, std::string_view right)
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](unsigned char lhs, unsigned char rhs) {
                          return std::tolower(lhs) == std::tolower(rhs);
                      });
}

std::string_view trimHttpWhitespace(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

std::size_t contentLength(const std::string& request, std::size_t headerEnd)
{
    const std::string_view headers(request.data(), headerEnd);
    std::size_t lineStart = headers.find("\r\n");
    if (lineStart == std::string_view::npos) {
        return 0;
    }
    lineStart += 2;

    while (lineStart < headers.size()) {
        const std::size_t lineEnd = headers.find("\r\n", lineStart);
        const std::string_view line = headers.substr(
            lineStart, (lineEnd == std::string_view::npos ? headers.size() : lineEnd) - lineStart);
        const std::size_t separator = line.find(':');
        if (separator != std::string_view::npos &&
            equalsIgnoreCase(trimHttpWhitespace(line.substr(0, separator)), "content-length")) {
            try {
                const auto value = std::stoull(std::string(trimHttpWhitespace(line.substr(separator + 1))));
                constexpr std::size_t kMaximumRequestBodyBytes = 1024 * 1024;
                return value <= kMaximumRequestBodyBytes ? static_cast<std::size_t>(value) : 0;
            } catch (const std::exception&) {
                return 0;
            }
        }
        if (lineEnd == std::string_view::npos) {
            break;
        }
        lineStart = lineEnd + 2;
    }
    return 0;
}

std::string receiveHttpRequest(int client)
{
    constexpr std::size_t kMaximumRequestBytes = 1024 * 1024;
    std::string request;
    char buffer[4096];
    std::size_t headerEnd = std::string::npos;
    std::size_t expectedBodyBytes = 0;

    while (request.size() < kMaximumRequestBytes) {
        const auto count = ::recv(client, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            return request;
        }
        request.append(buffer, static_cast<std::size_t>(count));

        if (headerEnd == std::string::npos) {
            headerEnd = request.find("\r\n\r\n");
            if (headerEnd == std::string::npos) {
                continue;
            }
            expectedBodyBytes = contentLength(request, headerEnd);
        }

        const std::size_t receivedBodyBytes = request.size() - (headerEnd + 4);
        if (receivedBodyBytes >= expectedBodyBytes) {
            return request;
        }
    }
    return request;
}

bool isCanonicalJsonPost(const std::string& request,
                         std::string_view expectedPath,
                         std::string_view expectedBody)
{
    const std::size_t headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return false;
    }
    const std::string_view body(request.data() + headerEnd + 4,
                                request.size() - headerEnd - 4);
    if (body != expectedBody) {
        return false;
    }

    const std::string_view headers(request.data(), headerEnd);
    const std::size_t requestLineEnd = headers.find("\r\n");
    const std::string expectedRequestLine =
        "POST " + std::string(expectedPath) + " HTTP/1.1";
    if (requestLineEnd == std::string_view::npos ||
        headers.substr(0, requestLineEnd) != std::string_view(expectedRequestLine)) {
        return false;
    }

    std::size_t contentTypeCount = 0;
    std::string_view contentType;
    std::size_t lineStart = requestLineEnd + 2;
    while (lineStart < headers.size()) {
        const std::size_t lineEnd = headers.find("\r\n", lineStart);
        const std::string_view line = headers.substr(
            lineStart, (lineEnd == std::string_view::npos ? headers.size() : lineEnd) - lineStart);
        const std::size_t separator = line.find(':');
        if (separator != std::string_view::npos &&
            equalsIgnoreCase(trimHttpWhitespace(line.substr(0, separator)), "content-type")) {
            ++contentTypeCount;
            contentType = trimHttpWhitespace(line.substr(separator + 1));
        }
        if (lineEnd == std::string_view::npos) {
            break;
        }
        lineStart = lineEnd + 2;
    }

    return contentTypeCount == 1 && contentType == "application/json";
}

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

    std::string firstRequest() const
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        return firstRequest_;
    }

  private:
    void serve(int fd)
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

            const std::string request = receiveHttpRequest(client);
            if (served == 0) {
                std::lock_guard<std::mutex> lock(requestMutex_);
                firstRequest_ = request;
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
    mutable std::mutex requestMutex_;
    std::string firstRequest_;
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
// Drogon's deadlock assertion.  The account POST additionally captures its
// loopback wire request so JSON Content-Type handling cannot regress.
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
            accountRequest.method = account::HttpMethod::Post;
            accountRequest.path = "/account-json";
            // The odd capitalization ensures the adapter honors field-name
            // case insensitivity while emitting one canonical Content-Type.
            accountRequest.headers["cOnTeNt-TyPe"] = "application/json";
            accountRequest.body = R"({"fixture":"account"})";
            const auto accountResult = account::makeDrogonAccountHttpTransport()->send(
                baseUrl, accountRequest, 2.0);
            const bool accountOk = accountResult.first == account::HttpResultCode::Ok &&
                                   accountResult.second &&
                                   accountResult.second->statusCode == 200;
            const bool accountRequestOk = isCanonicalJsonPost(
                server.firstRequest(), accountRequest.path, accountRequest.body);

            const bool chaynsOk = isOk(
                chayns::makeDrogonChaynsHttpTransport()->send(baseUrl, makeRequest(), 2.0));
            const bool retoolOk = isOk(
                retool::makeDrogonRetoolHttpTransport()->send(baseUrl, makeRequest(), 2.0));

            transportsReady = accountOk && accountRequestOk && chaynsOk && retoolOk;
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
