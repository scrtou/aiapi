#include <apipoint/chaynsapi/chaynsThreadReaper.h>

#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>
#include <ctime>

#include <apiManager/ApiManager.h>
#include <dbManager/chaynsThread/chaynsThreadDbManager.h>
#include <apipoint/chaynsapi/chaynsapi.h>

chaynsThreadReaper& chaynsThreadReaper::getInstance()
{
    static chaynsThreadReaper instance;
    return instance;
}

chaynsThreadReaper::~chaynsThreadReaper()
{
    stop();
}

chaynsThreadReaper::Options chaynsThreadReaper::getOptions() const
{
    std::lock_guard<std::mutex> lock(optionsMutex_);
    return options_;
}

void chaynsThreadReaper::start(const Options& options)
{
    {
        std::lock_guard<std::mutex> lock(optionsMutex_);
        options_ = options;
        // 参数兜底：配置写错（0 或负数）会让扫描退化成忙循环或永不触发，
        // 这里统一钳到安全下界，宁可慢也不能把 CPU 或上游打爆。
        options_.scanIntervalSeconds = std::max(options_.scanIntervalSeconds, 10);
        options_.idleSeconds         = std::max(options_.idleSeconds, 60);
        options_.batchLimit          = std::max(options_.batchLimit, 1);
        options_.maxAttempts         = std::max(options_.maxAttempts, 1);
        options_.deleteSpacingMs     = std::max(options_.deleteSpacingMs, 0);
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        LOG_INFO << "[chayns线程回收] 已在运行，仅更新参数";
        return;
    }

    stopRequested_ = false;
    worker_ = std::thread([this]() { loop(); });

    const Options applied = getOptions();
    LOG_INFO << "[chayns线程回收] 已启动：扫描间隔=" << applied.scanIntervalSeconds
             << "s, 空闲阈值=" << applied.idleSeconds
             << "s, 单轮上限=" << applied.batchLimit
             << ", 最大重试=" << applied.maxAttempts;
}

void chaynsThreadReaper::stop()
{
    if (!running_.load()) {
        return;
    }
    {
        // 置位必须与 loop() 的 wait_for 共用 wakeMutex_：谓词读的是同一个
        // stopRequested_，若在锁外置位，worker 可能已检查完谓词但尚未真正
        // 进入等待，此时 notify 落空，停机退化成等满一个 scanIntervalSeconds
        // （默认 15 分钟，测试里是 1 小时），表现为 join 永久挂起。
        // 同样的约束已在 AccountManager 与 chatSession 的停机路径上落实。
        std::lock_guard<std::mutex> lock(wakeMutex_);
        stopRequested_.store(true);
    }
    wakeCv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    running_ = false;
}

void chaynsThreadReaper::loop()
{
    while (!stopRequested_.load()) {
        // 先睡后干：进程刚起来时账号池/模型目录可能还没就绪，
        // 立刻扫描既没有收益，还容易在启动期打出一堆账号不可用的告警。
        {
            std::unique_lock<std::mutex> lock(wakeMutex_);
            const int waitSeconds = getOptions().scanIntervalSeconds;
            wakeCv_.wait_for(lock, std::chrono::seconds(waitSeconds),
                             [this]() { return stopRequested_.load(); });
        }
        if (stopRequested_.load()) {
            break;
        }

        try {
            const int removed = runOnce();
            if (removed > 0) {
                LOG_INFO << "[chayns线程回收] 本轮已回收上游线程 " << removed << " 个";
            }
        } catch (const std::exception& e) {
            // 回收是尽力而为的后台任务：任何异常都不能让线程退出，
            // 否则一次偶发 DB/网络抖动就等于永久关闭了回收能力。
            LOG_WARN << "[chayns线程回收] 本轮异常，已跳过：" << e.what();
        } catch (...) {
            LOG_WARN << "[chayns线程回收] 本轮未知异常，已跳过";
        }
    }
    LOG_INFO << "[chayns线程回收] 已停止";
}

int chaynsThreadReaper::runOnce()
{
    auto threadDb = chaynsThreadDbManager::getInstance();
    if (!threadDb || !threadDb->isEnabled()) {
        return 0;
    }

    const Options opt = getOptions();

    // 先清理重试耗尽的死行，避免它们持续占用每轮的 batchLimit 名额，
    // 把真正可回收的新行一直挤在后面。
    std::string purgeErr;
    const int purged = threadDb->purgeExhaustedThreads(opt.maxAttempts, &purgeErr);
    if (purged < 0) {
        LOG_WARN << "[chayns线程回收] 清理耗尽行失败：" << purgeErr;
    } else if (purged > 0) {
        LOG_WARN << "[chayns线程回收] 放弃 " << purged
                 << " 个重试耗尽的上游线程（可能已被上游删除或账号失效）";
    }

    const int64_t cutoff = static_cast<int64_t>(time(nullptr)) - opt.idleSeconds;
    std::string loadErr;
    auto rows = threadDb->loadThreadsOlderThan(cutoff, opt.batchLimit, &loadErr);
    if (rows.empty()) {
        if (!loadErr.empty()) {
            LOG_WARN << "[chayns线程回收] 读取待回收行失败：" << loadErr;
        }
        return 0;
    }

    auto api = ApiManager::getInstance().getApiByApiName("chaynsapi");
    auto chayns = std::dynamic_pointer_cast<chaynsapi>(api);
    if (!chayns) {
        // 台账存在但 chaynsapi 未注册（例如该实例关闭了 chayns 渠道）：
        // 此时无权也无法删上游，保留行等待有能力的实例处理，绝不静默删表。
        LOG_WARN << "[chayns线程回收] chaynsapi 未注册，跳过本轮上游删除";
        return 0;
    }

    int succeeded = 0;
    for (const auto& row : rows) {
        if (stopRequested_.load()) {
            break;
        }
        if (row.threadId.empty() || row.accountUserName.empty()) {
            // 缺少删除上下文的行无法补救，直接丢弃，否则它会永远卡在扫描窗口里。
            threadDb->deleteThread(row.threadId);
            continue;
        }

        bool ok = false;
        try {
            ok = chayns->deleteUpstreamThread(row.accountUserName, row.threadId,
                                              row.origin, row.referer);
        } catch (const std::exception& e) {
            ok = false;
            LOG_WARN << "[chayns线程回收] 删除上游线程异常：" << e.what();
        } catch (...) {
            ok = false;
            LOG_WARN << "[chayns线程回收] 删除上游线程未知异常";
        }

        if (ok) {
            // 只有上游确认删除（200/204）后才移除台账行，保证"行消失 == 远端已回收"。
            // 失败路径走 bumpDeleteAttempts，由重试计数兜底，不会静默丢资源。
            std::string delErr;
            if (!threadDb->deleteThread(row.threadId, &delErr)) {
                LOG_WARN << "[chayns线程回收] 台账行删除失败：" << delErr;
            } else {
                ++succeeded;
            }
        } else {
            threadDb->bumpDeleteAttempts(row.threadId);
        }

        if (opt.deleteSpacingMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.deleteSpacingMs));
        }
    }
    return succeeded;
}
