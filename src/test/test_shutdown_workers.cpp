#include <drogon/drogon_test.h>

#include <application/account/accountManager.h>
#include <infrastructure/provider/chayns/chaynsThreadReaper.h>
#include <infrastructure/provider/chayns/ChaynsHttpTransport.h>
#include <infrastructure/provider/chayns/chaynsapi.h>
#include <domain/port/IAccountStore.h>
#include <infrastructure/provider/ProviderRegistry.h>
#include <application/generation/core/Session.h>

#include <test/chayns_thread_stub_control.h>

#include <atomic>
#include <chrono>
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

DROGON_TEST(ShutdownWorkers_LongWaitsAreInterruptibleAndStopsAreIdempotent)
{
    using namespace std::chrono;
    const auto stoppedFast = [](const steady_clock::time_point start) {
        return duration_cast<milliseconds>(steady_clock::now() - start).count() < 1000;
    };

    AccountManager accounts;
    accounts.waitUpdateAccountTokenThread();
    accounts.checkAccountTypeThread();
    auto started = steady_clock::now();
    accounts.stopBackgroundThreads();
    CHECK(stoppedFast(started));
    accounts.stopBackgroundThreads();

    chatSession sessions;
    sessions.setCleanupIntervalSeconds(3600);
    sessions.startClearExpiredSession();
    started = steady_clock::now();
    sessions.stopClearExpiredSession();
    CHECK(stoppedFast(started));
    sessions.stopClearExpiredSession();

    chaynsThreadReaper::Options options;
    options.scanIntervalSeconds = 3600;
    auto ledger = std::make_shared<chaynsThreadDbManager>();
    chaynsThreadReaper reaper(ledger);
    provider::ProviderRegistry registry;
    reaper.setProviderRegistry(&registry);
    reaper.start(options);
    started = steady_clock::now();
    reaper.stop();
    CHECK(stoppedFast(started));
    reaper.stop();
}

// 本用例覆盖 reaper 限时 stop 的正常路径、过期 deadline 的快速返回和二次收割。
// 不可撤回的上游 DELETE 由独立的 spacing 用例覆盖；本用例不把网络时序混入断言。
DROGON_TEST(ShutdownWorkers_ReaperDeadlineOverloadStopsAndIsIdempotent)
{
    using namespace std::chrono;

    chaynsThreadReaper::Options options;
    options.scanIntervalSeconds = 3600;  // 若唤醒失效，join 会挂满一小时
    auto ledger = std::make_shared<chaynsThreadDbManager>();
    chaynsThreadReaper reaper(ledger);
    provider::ProviderRegistry registry;
    reaper.setProviderRegistry(&registry);
    reaper.start(options);

    const auto started = steady_clock::now();
    (void)reaper.stop(started + seconds(7));
    CHECK(duration_cast<milliseconds>(steady_clock::now() - started).count() < 1000);

    // 已停机后重复调用（两个重载混用）都必须直接返回，不得二次 join。
    (void)reaper.stop(steady_clock::now() + seconds(7));
    reaper.stop();

    // 已超支的 deadline 是真实场景（前面几段吃光了预算）：不得把负数丢进等待原语，
    // 也不得因为超支就跳过 join 而泄漏线程。
    reaper.start(options);
    const auto second = steady_clock::now();
    (void)reaper.stop(second - seconds(3));
    CHECK(duration_cast<milliseconds>(steady_clock::now() - second).count() < 1000);
    // 过期预算可能在 worker 收到唤醒前就耗尽；限时路径会刻意留下
    // joinable 线程，此处必须显式用无参重载收割，避免把残局带入后续用例。
    reaper.stop();
}

namespace {

// 只负责“让上游 DELETE 立即成功返回”的最小 transport。
// 本用例要测的是停机中断，不是 HTTP 行为；若让它慢，耗时就混入了
// 网络维度，断言会变成噪声。因此它必须瞬时返回，
// 使循环耗时完全由 deleteSpacingMs 限速等待主导。
class InstantDeleteTransport final : public chayns::IChaynsHttpTransport
{
  public:
    chayns::HttpResult send(const std::string&,
                            const drogon::HttpRequestPtr&,
                            double) override
    {
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k204NoContent);
        ++calls;
        return {drogon::ReqResult::Ok, response};
    }

    std::atomic<int> calls{0};
};

class ReaperAccountStore final : public IAccountStore
{
  public:
    std::list<Accountinfo_st> rows;

    bool addAccount(Accountinfo_st) override { return true; }
    bool updateAccount(Accountinfo_st) override { return true; }
    bool deleteAccount(std::string, std::string) override { return true; }
    bool isTableExist() override { return true; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    std::list<Accountinfo_st> getAccountDBList() override { return rows; }
    int createWaitingAccount(std::string) override { return 0; }
    bool activateAccount(int, Accountinfo_st) override { return true; }
    bool deleteWaitingAccount(int) override { return true; }
    int countAccountsByChannel(std::string, bool) override { return 0; }
    bool updateAccountStatusById(int, std::string) override { return true; }
    std::string getAccountStatusByUsername(std::string, std::string) override
    {
        return AccountStatus::ACTIVE;
    }
};

}  // namespace

/*
 * D5：限速等待必须可被停机打断。
 *
 * 背景（D4 的教训）：之前的两个 reaper 用例只观测“stop 是否快速返回”，
 * 而 worker 当时正睡在扫描周期上，根本没跑到删除循环——把
 * interruptibleSleepFor 退回不可中断的 sleep_for（变异 M2），用例照样全绿。
 *
 * 本用例强制让 runOnce() 停在删除循环里：注入 6 行待回收，
 * deleteSpacingMs = 3000。若限速不可中断，单轮至少 15s；可中断则应
 * 在毫秒级收尾。两条断言缺一不可：
 *   · 耗时上限——钉住“等待确实被 notify 打断”；
 *   · 已处理行数 < 总行数——钉住“确实提前退出”，而不是因为
 *     限速被改成 0 而飞快跑完。
 */
DROGON_TEST(ShutdownWorkers_ReaperDeleteLoopSpacingIsInterruptible)
{
    using namespace std::chrono;

    chaynsThreadStubControl::instance().reset();

    // runOnce() 第一道闸门是 isEnabled()，它是头文件内联函数，
    // 链接期桩替换覆盖不到，且 enabled_ 默认 false。
    // 不打开它，删除循环永远不可达（这正是 D4 变异杀不死的根因）。
    auto threadDb = std::make_shared<chaynsThreadDbManager>();
    threadDb->setEnabled(true);

    auto accountStore = std::make_shared<ReaperAccountStore>();
    Accountinfo_st account;
    account.apiName = "chaynsapi";
    account.userName = "reaper-fixture-account";
    account.authToken = "fixture-token";
    account.personId = "fixture-person";
    account.tokenStatus = true;
    account.accountStatus = true;
    account.status = AccountStatus::ACTIVE;
    accountStore->rows = {account};

    AccountManager accounts;
    accounts.setStore(accountStore);
    accounts.loadAccount();

    auto transport = std::make_shared<InstantDeleteTransport>();
    auto provider = std::make_shared<chaynsapi>(
        accounts, transport, chayns::makeRealChaynsClock(), threadDb);
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerChatProvider("chaynsapi", provider, provider, provider));

    std::vector<chaynsThreadDbManager::ThreadRow> rows;
    for (int i = 0; i < 6; ++i) {
        chaynsThreadDbManager::ThreadRow row;
        row.threadId = "thread-" + std::to_string(i);
        row.accountUserName = account.userName;
        rows.push_back(row);
    }
    chaynsThreadStubControl::instance().setRows(rows);

    chaynsThreadReaper::Options options;
    options.scanIntervalSeconds = 3600;
    options.batchLimit = 10;
    options.deleteSpacingMs = 3000;  // 不可中断时单轮 ≥ 15s
    chaynsThreadReaper reaper(threadDb);
    reaper.setProviderRegistry(&registry);
    reaper.start(options);

    std::atomic<bool> passDone{false};
    std::thread pass([&reaper, &passDone]() {
        reaper.runOnce();
        passDone.store(true);
    });

    // 等第一行真的被处理，确保已进入限速等待（而不是还没开始）。
    const auto spinStart = steady_clock::now();
    while (chaynsThreadStubControl::instance().deletedIds().empty() &&
           steady_clock::now() - spinStart < seconds(5)) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(!chaynsThreadStubControl::instance().deletedIds().empty());

    const auto started = steady_clock::now();
    (void)reaper.stop(started + seconds(7));
    pass.join();
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - started);

    CHECK(passDone.load());
    CHECK(elapsed.count() < 2000);
    CHECK(chaynsThreadStubControl::instance().deletedIds().size() < rows.size());

    threadDb->setEnabled(false);
    chaynsThreadStubControl::instance().reset();
}

namespace {

// 只记调用次数并一律返回非 200，使 isServerReachable 判定「不通」而继续重试。
// 它是本轮唯一可靠的探针：账号停机路径上没有其他可观测副作用。
class CountingAccountTransport final : public account::IAccountHttpTransport
{
  public:
    account::HttpResult send(const std::string&,
                             const account::HttpRequest&,
                             double) override
    {
        ++calls;
        auto response = std::make_shared<account::HttpResponse>();
        response->statusCode = 503;
        return {account::HttpResultCode::Ok, std::move(response)};
    }

    std::atomic<int> calls{0};
};

}  // namespace

/*
 * D11-T1：isServerReachable 的重试循环必须响应停机。
 *
 * 改造前该循环只看 retryCount < maxRetries，默认 maxRetries=300、每轮最坏
 * 3 个探测路径 x 30s 超时 + 1s 间隔，合计约 7.6 小时；而它跑在
 * waitUpdateAccountToken 工作线程上（updatechaynsToken -> getchaynsToken -> 此处），
 * 正是停机要 join 的线程之一。
 *
 * 强断言是 calls == 0，不是耗时：
 *   · 耗时断言杀不死「删掉循环头的停机检查」这个变异——因为间隔等待已改成
 *     backgroundSleep，停机态下它立即返回 false 并 break，耗时同样是毫秒级。
 *   · 但少了循环头检查，第一轮会先把 3 个探测路径全发出去才走到 break，
 *     transport 调用次数就从 0 变成 3。计数是唯一能分辨两者的观测点。
 *
 * backgroundStopRequested_ 是单向粘滞标志，因此本用例先在自己的局部
 * AccountManager 上请求停机，再观察循环不会再发出探测请求。
 */
DROGON_TEST(ShutdownWorkers_ServerReachabilityRetryLoopStopsImmediately)
{
    using namespace std::chrono;

    AccountManager accounts;
    // 幂等调用，只为确保本地 manager 的停机标志已置位。
    accounts.stopBackgroundThreads();

    auto transport = std::make_shared<CountingAccountTransport>();
    accounts.setHttpTransport(transport);

    // maxRetries 显式给 3 而不用默认 300：真要跑满，本用例自己就得等 4 分半。
    const auto started = steady_clock::now();
    const bool reachable = accounts.isServerReachable("http://127.0.0.1:9", 3);
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - started);

    CHECK(!reachable);
    CHECK(elapsed.count() < 1000);
    CHECK(transport->calls.load() == 0);
}

/*
 * D11-T2：updateAllAccountTypes 的逐账号循环必须响应停机。
 *
 * 改前循环体是「单次上游请求 + 500ms 不可中断节流」，耗时与账号数成正比、
 * 无常量上界，且要跑到自然结束才回到 while 头看停机标志。8 个账号即 4s，
 * 生产环境账号数一多就把停机拖成分钟级。
 *
 * 如实标注本用例的边界——它比 T1 弱：
 *   · 能杀死「把 500ms 节流退回 clock_->sleepFor / this_thread::sleep_for」
 *     这个变异（不可中断则至少 4s，断言 < 1000ms 会失败）。
 *   · 杀不死「删掉循环头的停机检查」：updateAccountType 目前首行就无条件
 *     return（见待议项 D11-Q3），整个函数体不可达，故循环体除 500ms 等待外
 *     没有任何可观测副作用，无法分辨「处理了 0 个账号」与「处理了 1 个」。
 *     若日后 D11-Q3 被修掉、该函数恢复出网，应把断言改强为 transport 调用计数。
 */
DROGON_TEST(ShutdownWorkers_AccountTypeRefreshLoopStopsImmediately)
{
    using namespace std::chrono;

    auto store = std::make_shared<ReaperAccountStore>();
    for (int i = 0; i < 8; ++i) {
        Accountinfo_st account;
        account.apiName       = "chaynsapi";
        account.userName      = "type-refresh-" + std::to_string(i);
        account.authToken     = "fixture-token";  // 空 token 会被快照过滤掉
        account.personId      = "fixture-person";
        account.tokenStatus   = true;             // 快照只收 tokenStatus 为真的账号
        account.accountStatus = true;
        account.status        = AccountStatus::ACTIVE;
        store->rows.push_back(account);
    }

    AccountManager accounts;
    accounts.setStore(store);
    accounts.loadAccount();
    accounts.setHttpTransport(std::make_shared<CountingAccountTransport>());
    accounts.stopBackgroundThreads();

    // 先确认账号真的进了 accountList：否则快照为空，下面的耗时断言恒成立。
    std::shared_ptr<Accountinfo_st> loaded;
    accounts.getAccountByUserName("chaynsapi", "type-refresh-0", loaded);
    REQUIRE(loaded != nullptr);

    const auto started = steady_clock::now();
    accounts.updateAllAccountTypes();
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - started);

    CHECK(elapsed.count() < 1000);
}

/*
 * D11 变异验证记录（2026-08-10，两个变异逐个施加后单跑，均已还原）：
 *
 *   M1  isServerReachable 的循环头去掉 !backgroundStopRequested_ 检查
 *       -> T1 第 3 条断言 transport->calls == 0 失败（实际发出 3 次探测）。
 *       杀死。这条断言正是 T1 的价值所在：耗时断言对 M1 无感，
 *       因为间隔等待已是 backgroundSleep，停机态下照样毫秒级返回。
 *
 *   M2  updateAllAccountTypes 的循环体退回「无停机检查 +
 *       std::this_thread::sleep_for(500ms)」
 *       -> T2 的耗时断言失败（8 个账号 x 500ms = 4s）。杀死。
 *
 * 已知未被覆盖的变异（如实记录，不假装覆盖）：
 *   M3  只删 updateAllAccountTypes 循环头的停机检查、保留 backgroundSleep
 *       -> T2 仍通过。根因是 updateAccountType 首行无条件 return（待议项 D11-Q3），
 *       循环体除等待外无可观测副作用。该待议项修掉后必须回来把 T2 改强。
 */
