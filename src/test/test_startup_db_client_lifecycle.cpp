#include <drogon/drogon_test.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <string>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct FixtureResult
{
    bool exited = false;
    int exitCode = -1;
    std::string output;
};

FixtureResult runStartupFixture(const std::string& fixtureName)
{
    FixtureResult result;
    int outputPipe[2];
    if (::pipe(outputPipe) != 0) return result;

    const auto executable =
        std::filesystem::canonical("/proc/self/exe").parent_path() /
        fixtureName;
    const pid_t child = ::fork();
    if (child == 0) {
        ::dup2(outputPipe[1], STDOUT_FILENO);
        ::close(outputPipe[0]);
        ::close(outputPipe[1]);
        ::execl(executable.c_str(), executable.c_str(), nullptr);
        ::_exit(127);
    }
    ::close(outputPipe[1]);
    if (child < 0) {
        ::close(outputPipe[0]);
        return result;
    }

    using namespace std::chrono;
    const auto deadline = steady_clock::now() + seconds(5);
    int status = 0;
    while (steady_clock::now() < deadline) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            result.exited = true;
            if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
            break;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    if (!result.exited) {
        ::kill(child, SIGKILL);
        ::waitpid(child, &status, 0);
    }

    char buffer[256];
    while (true) {
        const auto count = ::read(outputPipe[0], buffer, sizeof(buffer));
        if (count > 0) result.output.append(buffer, static_cast<size_t>(count));
        else break;
    }
    ::close(outputPipe[0]);
    return result;
}

FixtureResult runStartupDbClientFixture()
{
    return runStartupFixture("aiapi_startup_db_client_fixture");
}

}  // namespace

DROGON_TEST(StartupDbClient_IsAvailableAtBeginningAdvice)
{
    const auto result = runStartupDbClientFixture();
    CHECK(result.exited);
    CHECK(result.exitCode == 0);
    CHECK(result.output.find("DB_CLIENT_READY") != std::string::npos);
}

DROGON_TEST(StartupSyncHttp_ProductionTransportsUseANonCurrentIoLoop)
{
    const auto result = runStartupFixture("aiapi_startup_sync_http_fixture");
    CHECK(result.exited);
    CHECK(result.exitCode == 0);
    CHECK(result.output.find("SYNC_HTTP_TRANSPORTS_READY") != std::string::npos);
}
