#pragma once

#include <runtime/StartupResult.h>
#include <domain/port/IProviderRegistry.h>
#include <domain/port/IAiApiUseCase.h>
#include <domain/port/IResponseIndex.h>
#include <domain/port/IExecutionGate.h>
#include <domain/port/IAccountAdminUseCase.h>
#include <domain/port/IChannelAdminUseCase.h>
#include <domain/port/IHealthUseCase.h>
#include <domain/port/IMetricsUseCase.h>
#include <domain/port/IRetoolWorkspaceAdminUseCase.h>
#include <domain/port/IRetoolWorkspaceUseCase.h>
#include <infrastructure/managedAccount/contracts/ManagedAccount.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class chatSession;
class chaynsThreadReaper;
class BackgroundTaskQueue;
class IBackgroundExecutor;
class SessionDbManager;
class chaynsThreadDbManager;
class AccountDbManager;
class AccountBackupDbManager;
class AccountManager;
class ChannelDbManager;
class ChannelManager;
class ConfigDbManager;
class RetoolWorkspaceDbManager;
class RetoolWorkspaceManager;

namespace metrics {
class ErrorStatsDbManager;
class StatusDbManager;
class ErrorStatsService;
}  // namespace metrics

namespace lifecycle {

/**
 * @brief 一个具名的启动步骤。
 *
 * 之所以把步骤建模成数据而不是 `build()` 里的一串直写语句：G5 要求失败时能
 * 逆序 teardown 已完成的部分，而「已完成到第几步」只有在步骤可枚举时才是
 * 可判定的。名字进日志与失败 detail，使运维能直接定位到是哪一步崩的。
 */
struct StartupStep
{
    std::string                     name;
    std::function<StartupResult()>  run;
};

/**
 * @brief 一个后台 owner 的停机动作。
 *
 * `name` 不是装饰：停机日志此前是四条硬编码的 LOG_INFO，owner 一旦增删就与
 * 实际行为脱节。改成数据后日志由名字生成，不存在「说停了其实没停」的偏差。
 *
 * `stop` 收一个**绝对**截止时间点，而不是 `void()`：P4-W2 里 deadline 只到
 * `stopOwnersInReverse` 为止，owner 内部无从得知还剩多少时间，于是每个 owner 各
 * 自 join 到底——五个 owner 串起来，SIGTERM 宽限期被逐段突破（H3）。签名带上
 * deadline 后，「还剩多少」在任意一段里都能直接算出。
 *
 * 这里不保留 `void()` 重载：保留就等于允许新 owner 继续忽略 deadline，H1 会以
 * 「新代码走老路」的形式复发。让编译器强制每个 owner 都正面处理 deadline。
 */
struct RuntimeOwner
{
    std::string                                                name;
    std::function<void(std::chrono::steady_clock::time_point)> stop;
};

/**
 * @brief composition root 的持有者。
 *
 * 现状（P4-W2 之前）：`main.cc` 同时是配置加载器、CORS 装配点、12+ 个单例的
 * 注入器和 4 个后台 owner 的停机编排者，且初始化横跨 main 线程 → event loop
 * → 队列 worker 三个上下文（G2），`run()` 开始收请求时初始化未必完成。
 *
 * AppContext 把这三件事收敛成一条线性时序：
 *
 *     配置 → Drogon 创建 DB client → build() → 开放 listener → shutdown(deadline)
 *
 * Drogon 的 DB client 只能在 `run()` 内部创建，所以 production main 把 build()
 * 放在其 BeginningAdvice 中：它仍在主 event loop 同步跑完全部步骤，并且位于
 * listener 开放之前。因此对外可见的 happens-before 是构造性的，不再依赖
 * 「队列大概已经跑完了」这类假设。
 *
 * 本类不是单例：它由 main 在栈上持有，析构顺序因此是确定的（G7）。
 */
class AppContext
{
public:
    AppContext() = default;
    ~AppContext();

    AppContext(const AppContext&)            = delete;
    AppContext& operator=(const AppContext&) = delete;
    AppContext(AppContext&&)                 = delete;
    AppContext& operator=(AppContext&&)      = delete;

    /// 注册一个启动步骤。必须在 build() 之前调用；顺序即执行顺序。
    void addStep(std::string name, std::function<StartupResult()> run);

    /**
     * 注册一个后台 owner 的停机动作。
     *
     * 约定：谁启动谁登记，且**紧接启动之后**登记——这样 build() 中途失败时，
     * ownersStarted() 恰好等于「已经起来的那些」，逆序 stop 才是正确的回滚。
     */
    void addOwner(std::string name,
                  std::function<void(std::chrono::steady_clock::time_point)> stop);

    /**
     * 依次执行已注册步骤。
     *
     * - 步骤返回 Failed：立即停止推进，按登记逆序停掉已注册 owner，返回该失败。
     * - 步骤返回 Degraded：记入 degradedReasons() 并继续（G8 的两处有意降级）。
     * - 全部走完：Ok，或在存在降级时返回 Degraded 汇总。
     *
     * 重复调用返回 `AlreadyBuilt`——这是编程错误，不是环境错误，不应被重试逻辑吞掉。
     */
    [[nodiscard]] StartupResult build();

    /**
     * 按登记逆序停止全部 owner，deadline 为**绝对**时间点。
     *
     * 用绝对而非相对时长：停机要经过多个 owner，若每个各拿一份相对超时，总时长
     * 是各段之和，SIGTERM 宽限期会被逐段累加突破。绝对 deadline 让「还剩多少」
     * 在任意一段里都能直接算出。
     *
     * 幂等：二次调用为 no-op（G6）。
     */
    void shutdown(std::chrono::steady_clock::time_point deadline);

    bool isBuilt() const { return built_; }
    bool isShutdown() const { return shutdownDone_; }

    /// build() 期间记录的降级原因，供启动日志与 /ready 汇报使用。
    const std::vector<std::string>& degradedReasons() const { return degradedReasons_; }

    /// 已成功登记（即已启动）的 owner 数量，供回滚断言使用。
    size_t ownersStarted() const { return owners_.size(); }

    /// 已执行完成的步骤数量，失败时即为「失败步骤的下标」。
    size_t stepsCompleted() const { return stepsCompleted_; }

    /// Runtime-owned service published only after its providers are complete.
    void setProviderRegistry(std::shared_ptr<IProviderRegistry> registry)
    {
        providerRegistry_ = std::move(registry);
    }
    const std::shared_ptr<IProviderRegistry>& providerRegistry() const
    {
        return providerRegistry_;
    }
    void setHealthUseCase(std::shared_ptr<IHealthUseCase> useCase)
    {
        healthUseCase_ = std::move(useCase);
    }
    const std::shared_ptr<IHealthUseCase>& healthUseCase() const
    {
        return healthUseCase_;
    }
    void setAiApiUseCase(std::shared_ptr<aiapi::IAiApiUseCase> useCase)
    {
        aiApiUseCase_ = std::move(useCase);
    }
    const std::shared_ptr<aiapi::IAiApiUseCase>& aiApiUseCase() const
    {
        return aiApiUseCase_;
    }
    void setResponseIndex(std::shared_ptr<IResponseIndex> index) { responseIndex_ = std::move(index); }
    const std::shared_ptr<IResponseIndex>& responseIndex() const { return responseIndex_; }
    void setExecutionGate(std::shared_ptr<session::IExecutionGate> gate) { executionGate_ = std::move(gate); }
    const std::shared_ptr<session::IExecutionGate>& executionGate() const { return executionGate_; }
    /// The concrete queue is also the sole IBackgroundExecutor implementation
    /// published to application/transport.  Keeping both concepts on one
    /// context-owned object prevents an adapter singleton from outliving the
    /// queue it borrows.
    void setBackgroundTaskQueue(std::shared_ptr<BackgroundTaskQueue> queue)
    {
        backgroundTaskQueue_ = std::move(queue);
    }
    const std::shared_ptr<BackgroundTaskQueue>& backgroundTaskQueue() const
    {
        return backgroundTaskQueue_;
    }
    IBackgroundExecutor* backgroundExecutor() const;
    void setSessionPersistence(std::shared_ptr<SessionDbManager> persistence)
    {
        sessionPersistence_ = std::move(persistence);
    }
    const std::shared_ptr<SessionDbManager>& sessionPersistence() const
    {
        return sessionPersistence_;
    }
    void setThreadLedger(std::shared_ptr<chaynsThreadDbManager> ledger)
    {
        threadLedger_ = std::move(ledger);
    }
    const std::shared_ptr<chaynsThreadDbManager>& threadLedger() const
    {
        return threadLedger_;
    }
    void setErrorStatsStore(std::shared_ptr<metrics::ErrorStatsDbManager> store)
    {
        errorStatsStore_ = std::move(store);
    }
    const std::shared_ptr<metrics::ErrorStatsDbManager>& errorStatsStore() const
    {
        return errorStatsStore_;
    }
    void setStatusMetricsStore(std::shared_ptr<metrics::StatusDbManager> store)
    {
        statusMetricsStore_ = std::move(store);
    }
    const std::shared_ptr<metrics::StatusDbManager>& statusMetricsStore() const
    {
        return statusMetricsStore_;
    }
    void setErrorStatsService(std::shared_ptr<metrics::ErrorStatsService> service)
    {
        errorStatsService_ = std::move(service);
    }
    const std::shared_ptr<metrics::ErrorStatsService>& errorStatsService() const
    {
        return errorStatsService_;
    }
    void setMetricsUseCase(std::shared_ptr<metrics::IMetricsUseCase> useCase)
    {
        metricsUseCase_ = std::move(useCase);
    }
    const std::shared_ptr<metrics::IMetricsUseCase>& metricsUseCase() const
    {
        return metricsUseCase_;
    }
    void setChannelStore(std::shared_ptr<ChannelDbManager> store)
    {
        channelStore_ = std::move(store);
    }
    const std::shared_ptr<ChannelDbManager>& channelStore() const
    {
        return channelStore_;
    }
    void setChannelManager(std::shared_ptr<ChannelManager> channels)
    {
        channelManager_ = std::move(channels);
    }
    const std::shared_ptr<ChannelManager>& channelManager() const { return channelManager_; }
    void setConfigStore(std::shared_ptr<ConfigDbManager> store)
    {
        configStore_ = std::move(store);
    }
    const std::shared_ptr<ConfigDbManager>& configStore() const
    {
        return configStore_;
    }
    void setAccountStore(std::shared_ptr<AccountDbManager> store)
    {
        accountStore_ = std::move(store);
    }
    const std::shared_ptr<AccountDbManager>& accountStore() const
    {
        return accountStore_;
    }
    void setAccountBackupStore(std::shared_ptr<AccountBackupDbManager> store)
    {
        accountBackupStore_ = std::move(store);
    }
    const std::shared_ptr<AccountBackupDbManager>& accountBackupStore() const
    {
        return accountBackupStore_;
    }
    void setRetoolWorkspaceStore(std::shared_ptr<RetoolWorkspaceDbManager> store)
    {
        retoolWorkspaceStore_ = std::move(store);
    }
    const std::shared_ptr<RetoolWorkspaceDbManager>& retoolWorkspaceStore() const
    {
        return retoolWorkspaceStore_;
    }
    void setRetoolWorkspaceManager(std::shared_ptr<RetoolWorkspaceManager> manager)
    {
        retoolWorkspaceManager_ = std::move(manager);
    }
    const std::shared_ptr<RetoolWorkspaceManager>& retoolWorkspaceManager() const
    {
        return retoolWorkspaceManager_;
    }
    void setRetoolWorkspaceProvisioner(
        std::shared_ptr<workspace::IRetoolWorkspaceProvisioner> provisioner)
    {
        retoolWorkspaceProvisioner_ = std::move(provisioner);
    }
    const std::shared_ptr<workspace::IRetoolWorkspaceProvisioner>& retoolWorkspaceProvisioner() const
    {
        return retoolWorkspaceProvisioner_;
    }
    void setRetoolWorkspaceUseCase(std::shared_ptr<workspace::IRetoolWorkspaceUseCase> useCase)
    {
        retoolWorkspaceUseCase_ = std::move(useCase);
    }
    const std::shared_ptr<workspace::IRetoolWorkspaceUseCase>& retoolWorkspaceUseCase() const
    {
        return retoolWorkspaceUseCase_;
    }
    void setRetoolWorkspaceAdminUseCase(
        std::shared_ptr<workspace::IRetoolWorkspaceAdminUseCase> useCase)
    {
        retoolWorkspaceAdminUseCase_ = std::move(useCase);
    }
    const std::shared_ptr<workspace::IRetoolWorkspaceAdminUseCase>&
    retoolWorkspaceAdminUseCase() const
    {
        return retoolWorkspaceAdminUseCase_;
    }
    void setAccountManager(std::shared_ptr<AccountManager> accounts)
    {
        accountManager_ = std::move(accounts);
    }
    const std::shared_ptr<AccountManager>& accountManager() const { return accountManager_; }
    void setAccountAdminUseCase(std::shared_ptr<IAccountAdminUseCase> useCase)
    {
        accountAdminUseCase_ = std::move(useCase);
    }
    const std::shared_ptr<IAccountAdminUseCase>& accountAdminUseCase() const
    {
        return accountAdminUseCase_;
    }
    void setChannelAdminUseCase(std::shared_ptr<IChannelAdminUseCase> useCase)
    {
        channelAdminUseCase_ = std::move(useCase);
    }
    const std::shared_ptr<IChannelAdminUseCase>& channelAdminUseCase() const
    {
        return channelAdminUseCase_;
    }
    void setSessionStore(std::shared_ptr<chatSession> store) { sessionStore_ = std::move(store); }
    const std::shared_ptr<chatSession>& sessionStore() const { return sessionStore_; }
    void setThreadReaper(std::shared_ptr<chaynsThreadReaper> reaper)
    {
        threadReaper_ = std::move(reaper);
    }
    const std::shared_ptr<chaynsThreadReaper>& threadReaper() const { return threadReaper_; }
    void setManagedAccountService(std::shared_ptr<IManagedAccountContextResolver> service)
    {
        managedAccountService_ = std::move(service);
    }
    const std::shared_ptr<IManagedAccountContextResolver>& managedAccountService() const
    {
        return managedAccountService_;
    }

private:
    void stopOwnersInReverse(std::chrono::steady_clock::time_point deadline);

    std::vector<StartupStep>  steps_;
    std::vector<RuntimeOwner> owners_;
    std::vector<std::string>  degradedReasons_;
    size_t                    stepsCompleted_ = 0;
    bool                      built_          = false;
    bool                      shutdownDone_   = false;
    // 声明顺序决定逆序析构。队列必须最后析构，确保仍在执行的写穿任务
    // 不会访问已销毁的 DB manager；provider 必须先于其借用的 thread ledger。
    std::shared_ptr<BackgroundTaskQueue> backgroundTaskQueue_;
    std::shared_ptr<SessionDbManager> sessionPersistence_;
    std::shared_ptr<chaynsThreadDbManager> threadLedger_;
    // ErrorStatsService owns a sink reference and must be destroyed before the
    // concrete metrics stores.  Providers are declared afterwards so their
    // destruction may still emit final telemetry while this service is alive.
    std::shared_ptr<metrics::ErrorStatsDbManager> errorStatsStore_;
    std::shared_ptr<metrics::StatusDbManager> statusMetricsStore_;
    std::shared_ptr<metrics::ErrorStatsService> errorStatsService_;
    // MetricsController borrows this facade, which must disappear before the
    // context-owned query adapters above are destroyed.
    std::shared_ptr<metrics::IMetricsUseCase> metricsUseCase_;
    // Application services have no process singleton.  Concrete account,
    // channel, config and workspace stores all precede their borrowing
    // managers/use cases, so reverse destruction tears down AccountManager /
    // AccountAdminUseCase / workspace facades before their backing stores.
    std::shared_ptr<ChannelDbManager> channelStore_;
    std::shared_ptr<ChannelManager> channelManager_;
    std::shared_ptr<ConfigDbManager> configStore_;
    std::shared_ptr<AccountDbManager> accountStore_;
    std::shared_ptr<AccountBackupDbManager> accountBackupStore_;
    std::shared_ptr<RetoolWorkspaceDbManager> retoolWorkspaceStore_;
    std::shared_ptr<RetoolWorkspaceManager> retoolWorkspaceManager_;
    std::shared_ptr<workspace::IRetoolWorkspaceProvisioner> retoolWorkspaceProvisioner_;
    std::shared_ptr<workspace::IRetoolWorkspaceUseCase> retoolWorkspaceUseCase_;
    std::shared_ptr<workspace::IRetoolWorkspaceAdminUseCase> retoolWorkspaceAdminUseCase_;
    // AccountManager outlives all providers and the account application facade.
    // They are declared afterwards so reverse destruction tears them down first.
    std::shared_ptr<AccountManager> accountManager_;
    std::shared_ptr<IAccountAdminUseCase> accountAdminUseCase_;
    std::shared_ptr<IChannelAdminUseCase> channelAdminUseCase_;
    // Registry/Provider 必须先于其引用的账号服务销毁。
    std::shared_ptr<IManagedAccountContextResolver> managedAccountService_;
    std::shared_ptr<IProviderRegistry> providerRegistry_;
    std::shared_ptr<IHealthUseCase> healthUseCase_;
    std::shared_ptr<IResponseIndex> responseIndex_;
    std::shared_ptr<session::IExecutionGate> executionGate_;
    // SessionStore 借用上面各服务；其声明顺序保证先于所有借用目标析构。
    std::shared_ptr<chatSession> sessionStore_;
    // This facade borrows the context-owned collaborators above.  Declaring
    // it here releases it before those collaborators during reverse teardown.
    std::shared_ptr<aiapi::IAiApiUseCase> aiApiUseCase_;
    // 最后声明、最先析构：Reaper 借用 ProviderRegistry，必须先于它析构。
    std::shared_ptr<chaynsThreadReaper> threadReaper_;
};

}  // namespace lifecycle
