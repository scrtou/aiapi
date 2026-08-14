#include <drogon/drogon_test.h>

#include <domain/port/ISessionPersistence.h>
#include <sessionManager/core/Session.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono;

/**
 * A deliberately blocking persistence port for the session-janitor shutdown
 * path.  Its finite fallback makes a broken unlimited join fail by elapsed
 * time instead of hanging the whole test process forever.
 */
class BlockingSessionPersistence final : public ISessionPersistence
{
  public:
    bool ensureTables(std::string*) override { return true; }
    bool isEnabled() const override { return true; }
    void setEnabled(bool) override {}

    std::optional<SessionPersistenceRow> loadSession(const std::string&, std::string*) override
    {
        return std::nullopt;
    }
    std::optional<SessionPersistenceRow> loadSessionByContextKey(
        const std::string&, std::string*) override
    {
        return std::nullopt;
    }
    std::optional<ResponsePersistenceRow> loadResponse(const std::string&, std::string*) override
    {
        return std::nullopt;
    }

    void asyncUpsertSession(const SessionPersistenceRow&) override {}
    void asyncUpsertResponse(const ResponsePersistenceRow&) override {}
    void asyncDeleteSessions(const std::vector<std::string>&) override {}
    void asyncDeleteResponses(const std::vector<std::string>&) override {}

    int deleteSessionsOlderThan(int64_t, std::string*) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        enteredCv_.notify_all();
        // A finite fallback is intentional: a mutation which turns the
        // deadline-aware join back into thread.join() must fail quickly, not
        // strand the whole CTest invocation forever.
        releaseCv_.wait_for(lock, seconds(2), [this] { return released_; });
        return 0;
    }

    void rearm()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entered_ = false;
        released_ = false;
    }

    bool waitUntilEntered(milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return enteredCv_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        releaseCv_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable enteredCv_;
    std::condition_variable releaseCv_;
    bool entered_ = false;
    bool released_ = false;
};

}  // namespace

/*
 * P5-W3 session lifecycle closure: chatSession is now a normal object owned by
 * AppContext, so this test can hold an isolated instance and safely inject a
 * blocking persistence port.  It proves both of the previously untestable
 * deadline invariants:
 *
 *  - a DB cleanup already in flight makes the bounded stop return false rather
 *    than calling an unbounded join;
 *  - the second start receives a fresh ThreadCompletion.  Reusing the signal
 *    from the first worker would make joinUntil call thread.join() immediately
 *    and exceed the budget on the second blocked run.
 */
DROGON_TEST(SessionCleanerDeadlineLeavesBlockedWorkerForExplicitReap)
{
    constexpr auto kWaitForCleanup = seconds(3);
    constexpr auto kStopBudget = milliseconds(150);
    constexpr auto kPromptReturn = milliseconds(800);

    BlockingSessionPersistence persistence;
    chatSession sessions;
    sessions.setPersistence(&persistence);
    sessions.setPersistenceEnabled(true);
    // The production cleaner intentionally polls; one second is the smallest
    // legal interval and guarantees the worker reaches the fake DB boundary.
    sessions.setCleanupIntervalSeconds(1);

    const auto runBlockedStop = [&] {
        persistence.rearm();
        sessions.startClearExpiredSession();
        REQUIRE(persistence.waitUntilEntered(kWaitForCleanup));

        const auto started = steady_clock::now();
        const bool joined = sessions.stopClearExpiredSession(started + kStopBudget);
        const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - started);

        CHECK(!joined);
        CHECK(elapsed < kPromptReturn);
        CHECK(sessions.hasPendingCleaner());

        // The deadline path intentionally keeps the worker and object alive.
        // Once the fake DB operation returns, the no-arg path is responsible
        // for the final safe join.
        persistence.release();
        sessions.stopClearExpiredSession();
        CHECK(!sessions.hasPendingCleaner());
    };

    runBlockedStop();
    runBlockedStop();
}
