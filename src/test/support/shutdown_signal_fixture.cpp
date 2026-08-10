#include <drogon/drogon.h>

#include <runtime/AppContext.h>
#include <utils/BackgroundTaskQueue.h>

#include <chrono>
#include <csignal>
#include <iostream>

int main()
{
    BackgroundTaskQueue::instance().start(2);

    // 与 main.cc 同一套机制：owner 按启动顺序登记，AppContext 逆序停止。
    // fixture 必须复用生产代码里的 AppContext，而不是自己复述一份顺序——
    // 复述出来的顺序即使与生产不一致，测试也照样绿。
    lifecycle::AppContext appContext;
    appContext.addOwner("task-queue", [] {
        BackgroundTaskQueue::instance().shutdown();
        std::cout << "QUEUE" << std::endl;
    });
    appContext.addOwner("session-cleaner", [] { std::cout << "SESSION" << std::endl; });
    appContext.addOwner("account-workers", [] { std::cout << "ACCOUNTS" << std::endl; });
    appContext.addOwner("chayns-reaper", [] { std::cout << "REAPER" << std::endl; });

    // READY 必须在 Drogon 装好 SIGTERM handler *之后* 才打印，否则外部脚本
    // 一看到 READY 就发信号，会命中默认处置直接把进程杀掉（exit 143/124），
    // 表现为「停机挂起」的假阳性。runOnLoopStarted 早于 handler 安装，
    // 因此这里改用 loop 上的一次性定时器 + 显式 flush。
    drogon::app().getLoop()->runAfter(0.05, [] {
        std::cout << "READY" << std::endl;
        std::cout.flush();
    });

    // Drogon 的生产 SIGTERM handler 会调用 app().quit()；run() 返回后走与
    // main.cc 完全相同的停机路径。
    drogon::app().run();
    appContext.shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(25));
    std::cout << "EXIT" << std::endl;
    return 0;
}
