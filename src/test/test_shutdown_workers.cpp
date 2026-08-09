#include <drogon/drogon_test.h>

#include <accountManager/accountManager.h>
#include <apipoint/chaynsapi/chaynsThreadReaper.h>
#include <sessionManager/core/Session.h>

#include <chrono>

DROGON_TEST(ShutdownWorkers_LongWaitsAreInterruptibleAndStopsAreIdempotent)
{
    using namespace std::chrono;
    const auto stoppedFast = [](const steady_clock::time_point start) {
        return duration_cast<milliseconds>(steady_clock::now() - start).count() < 1000;
    };

    auto& accounts = AccountManager::getInstance();
    accounts.waitUpdateAccountTokenThread();
    accounts.checkAccountTypeThread();
    auto started = steady_clock::now();
    accounts.stopBackgroundThreads();
    CHECK(stoppedFast(started));
    accounts.stopBackgroundThreads();

    auto *sessions = chatSession::getInstance();
    sessions->setCleanupIntervalSeconds(3600);
    sessions->startClearExpiredSession();
    started = steady_clock::now();
    sessions->stopClearExpiredSession();
    CHECK(stoppedFast(started));
    sessions->stopClearExpiredSession();

    chaynsThreadReaper::Options options;
    options.scanIntervalSeconds = 3600;
    auto& reaper = chaynsThreadReaper::getInstance();
    reaper.start(options);
    started = steady_clock::now();
    reaper.stop();
    CHECK(stoppedFast(started));
    reaper.stop();
}
