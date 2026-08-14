// TSan 窄范围探针驱动。
// 重要前提（来自 reaper.cpp start()）：scanIntervalSeconds 被钳到 >=10，
// 且 loop() 先 wait_for 再执行循环体。因此任何短于 10s 的场景都只能观测到
// start/stop 的同步配对，观测不到 runOnce 路径 —— 必须显式区分这两类场景。
#include <apipoint/chaynsapi/chaynsThreadReaper.h>
#include <dbManager/chaynsThread/chaynsThreadDbManager.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
chaynsThreadReaper::Options fastOptions() {
    chaynsThreadReaper::Options opt;
    opt.scanIntervalSeconds = 10;  // 钳制下界，写清楚而不是假装能设 1
    opt.idleSeconds         = 60;
    opt.batchLimit          = 1;
    opt.maxAttempts         = 1;
    opt.deleteSpacingMs     = 0;
    return opt;
}
}  // namespace

int main() {
    using namespace std::chrono_literals;
    // The probe is a tiny composition root too: keep the ledger alive for the
    // whole reaper lifetime instead of relying on the production singleton.
    auto ledger = chaynsThreadDbManager::getInstance();
    ledger->setEnabled(true);
    chaynsThreadReaper reaper(ledger);

    // 场景 A：高频 start/stop，只压 stopRequested_/wakeCv_/worker_ 的同步配对。
    for (int i = 0; i < 300; ++i) {
        reaper.start(fastOptions());
        std::this_thread::sleep_for(std::chrono::microseconds(i % 11));
        reaper.stop();
    }
    std::printf("[probe] A done: 300 start/stop (only sync pairing observed)\n");

    // 场景 B：4 线程并发 runOnce()，直接压 options_ / enabled_ / DB 桩，
    // 不依赖 loop 自然唤醒。
    {
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&] { for (int i = 0; i < 200; ++i) reaper.runOnce(); });
        for (auto& t : ts) t.join();
    }
    std::printf("[probe] B done: 4x200 concurrent runOnce\n");

    // 场景 C：runOnce 与 setEnabled 并发（enabled_ 是裸 bool，这里是真正的读写并发）。
    {
        std::atomic<bool> stopFlag{false};
        std::thread flipper([&] {
            while (!stopFlag.load(std::memory_order_relaxed)) {
                ledger->setEnabled(true);
                ledger->setEnabled(false);
            }
        });
        for (int i = 0; i < 300; ++i) reaper.runOnce();
        stopFlag.store(true, std::memory_order_relaxed);
        flipper.join();
        ledger->setEnabled(true);
    }
    std::printf("[probe] C done: runOnce vs setEnabled\n");

    // 场景 D：唯一让 loop 循环体真正执行的场景 —— 必须等过 10s 钳制下界。
    reaper.start(fastOptions());
    std::printf("[probe] D: waiting 13s for loop body to execute at least once...\n");
    std::fflush(stdout);
    std::this_thread::sleep_for(13s);
    // loop 已至少跑完一轮循环体，此刻并发 stop + runOnce 制造交错
    std::thread racer([&] { for (int i = 0; i < 50; ++i) reaper.runOnce(); });
    reaper.stop();
    racer.join();
    std::printf("[probe] D done: loop body executed + concurrent stop/runOnce\n");

    // 场景 E：start(Options) 与 runOnce() 并发 —— 唯一能暴露 options_ 读写竞争的形态。
    // 前置结论（步骤12）：options_ 的写只在 start()，读在 getOptions()；
    // 若不让二者交错，摘掉 optionsMutex_ 也不会被 TSan 发现（M1 阴性即由此产生）。
    {
        std::atomic<bool> stopFlag{false};
        std::thread reconfigurer([&] {
            int n = 0;
            while (!stopFlag.load(std::memory_order_relaxed)) {
                auto opt = fastOptions();
                // 让每次写入的值都不同，便于 TSan 判定为真实写
                opt.batchLimit = 1 + (n++ % 7);
                reaper.start(opt);   // 已在运行时走「仅更新参数」分支，只写 options_
            }
        });
        std::vector<std::thread> readers;
        for (int t = 0; t < 3; ++t)
            readers.emplace_back([&] { for (int i = 0; i < 400; ++i) reaper.runOnce(); });
        for (auto& t : readers) t.join();
        stopFlag.store(true, std::memory_order_relaxed);
        reconfigurer.join();
    }
    std::printf("[probe] E done: concurrent start(Options) vs runOnce (options_ RW)\n");
    reaper.stop();
    return 0;
}
