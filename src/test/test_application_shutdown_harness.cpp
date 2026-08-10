#include <drogon/drogon_test.h>

#include <runtime/AppContext.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct ChildResult
{
    bool ready = false;
    bool exited = false;
    int exitCode = -1;
    std::string output;
};

bool readUntilReady(int fd, std::string& output)
{
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + seconds(3);
    char buffer[256];
    while (steady_clock::now() < deadline) {
        pollfd descriptor{fd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 50);
        if (ready > 0 && (descriptor.revents & POLLIN)) {
            const auto count = ::read(fd, buffer, sizeof(buffer));
            if (count > 0) {
                output.append(buffer, static_cast<size_t>(count));
                if (output.find("READY\n") != std::string::npos) return true;
            }
        }
    }
    return false;
}

ChildResult runSignalFixture()
{
    ChildResult result;
    int outputPipe[2];
    if (::pipe(outputPipe) != 0) return result;

    const auto executable =
        std::filesystem::canonical("/proc/self/exe").parent_path() /
        "aiapi_shutdown_signal_fixture";
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

    result.ready = readUntilReady(outputPipe[0], result.output);
    if (result.ready) ::kill(child, SIGTERM);

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

    char buffer[512];
    while (true) {
        const auto count = ::read(outputPipe[0], buffer, sizeof(buffer));
        if (count > 0) result.output.append(buffer, static_cast<size_t>(count));
        else break;
    }
    ::close(outputPipe[0]);
    return result;
}

}  // namespace

DROGON_TEST(ApplicationShutdown_StopsOwnersInReverseRegistrationOrder)
{
    // reverse-order is the invariant under test: registration order is startup
    // order, so shutdown must be its reverse and no second order table exists.
    std::vector<std::string> order;
    lifecycle::AppContext ctx;
    ctx.addOwner("task-queue", [&order](std::chrono::steady_clock::time_point) { order.push_back("queue"); });
    ctx.addOwner("session-cleaner", [&order](std::chrono::steady_clock::time_point) { order.push_back("session"); });
    ctx.addOwner("account-workers", [&order](std::chrono::steady_clock::time_point) { order.push_back("accounts"); });
    ctx.addOwner("chayns-reaper", [&order](std::chrono::steady_clock::time_point) { order.push_back("reaper"); });

    ctx.shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(5));

    const std::vector<std::string> expected{
        "reaper", "accounts", "session", "queue"};
    CHECK(order == expected);
}

DROGON_TEST(ApplicationShutdown_SecondShutdownIsNoOp)
{
    // G6 idempotency: a real second stop would double-join threads already
    // joined and shut down the queue twice.
    int stops = 0;
    lifecycle::AppContext ctx;
    ctx.addOwner("counted", [&stops](std::chrono::steady_clock::time_point) { ++stops; });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    ctx.shutdown(deadline);
    ctx.shutdown(deadline);

    CHECK(stops == 1);
    CHECK(ctx.isShutdown());
}

DROGON_TEST(ApplicationShutdown_PastDeadlineStillStopsEveryOwner)
{
    // The deadline records overrun; it never skips stop(). Skipping would leave
    // joins to be truncated at process exit -- the very bug this replaced.
    std::vector<std::string> order;
    lifecycle::AppContext ctx;
    ctx.addOwner("first", [&order](std::chrono::steady_clock::time_point) { order.push_back("first"); });
    ctx.addOwner("second", [&order](std::chrono::steady_clock::time_point) { order.push_back("second"); });

    ctx.shutdown(std::chrono::steady_clock::now() - std::chrono::seconds(1));

    const std::vector<std::string> expected{"second", "first"};
    CHECK(order == expected);
}

DROGON_TEST(ApplicationShutdown_IdleProcessHandlesSigtermInProductionOrder)
{
    const auto result = runSignalFixture();
    CHECK(result.ready);
    CHECK(result.exited);
    CHECK(result.exitCode == 0);

    const auto reaper = result.output.find("REAPER\n");
    const auto accounts = result.output.find("ACCOUNTS\n");
    const auto session = result.output.find("SESSION\n");
    const auto queue = result.output.find("QUEUE\n");
    const auto exit = result.output.find("EXIT\n");
    REQUIRE(reaper != std::string::npos);
    REQUIRE(accounts != std::string::npos);
    REQUIRE(session != std::string::npos);
    REQUIRE(queue != std::string::npos);
    REQUIRE(exit != std::string::npos);
    CHECK(reaper < accounts);
    CHECK(accounts < session);
    CHECK(session < queue);
    CHECK(queue < exit);
}
