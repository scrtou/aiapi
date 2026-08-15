#include <accountManager/accountManager.h>

#include <drogon/drogon.h>

#include <chrono>

using namespace drogon;

namespace {

struct CompletionSignaler
{
    explicit CompletionSignaler(platform::ThreadCompletionPtr completion)
        : completion_(std::move(completion))
    {
    }
    ~CompletionSignaler()
    {
        if (completion_) completion_->signal();
    }
    platform::ThreadCompletionPtr completion_;
};

}  // namespace

bool AccountManager::backgroundSleep(std::chrono::milliseconds duration)
{
    std::unique_lock<std::mutex> lock(backgroundWakeMutex_);
    const bool stopped = backgroundWakeCv_.wait_for(
        lock, duration, [this] { return backgroundStopRequested_.load(); });
    return !stopped;
}

void AccountManager::stopBackgroundThreads()
{
    (void)stopBackgroundThreads(std::chrono::steady_clock::time_point::max());
}

bool AccountManager::stopBackgroundThreads(std::chrono::steady_clock::time_point deadline)
{
    {
        std::lock_guard<std::mutex> lock(backgroundWakeMutex_);
        backgroundStopRequested_.store(true);
    }
    backgroundWakeCv_.notify_all();
    accountListNeedUpdateCondition.notify_all();

    bool allJoined = true;
    const auto join = [&](std::thread& thread, const platform::ThreadCompletionPtr& completion,
                          const char* name) {
        if (!thread.joinable()) return;
        bool joined = true;
        if (deadline != std::chrono::steady_clock::time_point::max() && completion) {
            joined = platform::joinUntil(thread, *completion, deadline);
        } else {
            thread.join();
        }
        if (!joined) {
            allJoined = false;
            LOG_WARN << "[账户管理] 后台线程未在停机预算内退出: " << name;
        }
    };
    join(tokenCheckThread_, tokenCheckDone_, "令牌巡检");
    join(tokenUpdateWorker_, tokenUpdateDone_, "令牌更新工作线程");
    join(accountCountThread_, accountCountDone_, "账号数量巡检");
    join(accountTypeThread_, accountTypeDone_, "账号类型巡检");
    if (allJoined) {
        tokenCheckDone_.reset();
        tokenUpdateDone_.reset();
        accountCountDone_.reset();
        accountTypeDone_.reset();
    }
    return allJoined;
}

void AccountManager::checkUpdateTokenthread()
{
    if (tokenCheckThread_.joinable()) {
        LOG_WARN << "[账户管理] 令牌巡检线程已在运行，忽略重复启动";
        return;
    }
    tokenCheckDone_ = std::make_shared<platform::ThreadCompletion>();
    tokenCheckThread_ = std::thread([this, done = tokenCheckDone_] {
        CompletionSignaler signaler(done);
        while (!backgroundStopRequested_.load()) {
            checkToken();
            cleanExpiredAccounts();
            if (!backgroundSleep(std::chrono::hours(5))) break;
        }
    });
}

void AccountManager::waitUpdateAccountTokenThread()
{
    if (tokenUpdateWorker_.joinable()) {
        LOG_WARN << "[账户管理] 令牌更新工作线程已在运行，忽略重复启动";
        return;
    }
    tokenUpdateDone_ = std::make_shared<platform::ThreadCompletion>();
    tokenUpdateWorker_ = std::thread([this, done = tokenUpdateDone_] {
        CompletionSignaler signaler(done);
        waitUpdateAccountToken();
    });
}

void AccountManager::checkAccountCountThread()
{
    if (accountCountThread_.joinable()) {
        LOG_WARN << "[账户管理] 账号数量巡检线程已在运行，忽略重复启动";
        return;
    }
    accountCountDone_ = std::make_shared<platform::ThreadCompletion>();
    accountCountThread_ = std::thread([this, done = accountCountDone_] {
        CompletionSignaler signaler(done);
        while (!backgroundStopRequested_.load()) {
            checkChannelAccountCounts();
            if (!backgroundSleep(std::chrono::minutes(10))) break;
        }
    });
}

void AccountManager::checkAccountTypeThread()
{
    if (accountTypeThread_.joinable()) {
        LOG_WARN << "[账户管理] 账号类型巡检线程已在运行，忽略重复启动";
        return;
    }
    accountTypeDone_ = std::make_shared<platform::ThreadCompletion>();
    accountTypeThread_ = std::thread([this, done = accountTypeDone_] {
        CompletionSignaler signaler(done);
        if (!backgroundSleep(std::chrono::minutes(1))) return;
        while (!backgroundStopRequested_.load()) {
            updateAllAccountTypes();
            if (!backgroundSleep(std::chrono::hours(3))) break;
        }
    });
}
