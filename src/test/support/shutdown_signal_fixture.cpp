#include <drogon/drogon.h>

#include <utils/ApplicationShutdown.h>
#include <utils/BackgroundTaskQueue.h>

#include <csignal>
#include <iostream>

int main()
{
    BackgroundTaskQueue::instance().start(2);
    // READY 必须在 Drogon 装好 SIGTERM handler *之后* 才打印，否则外部脚本
    // 一看到 READY 就发信号，会命中默认处置直接把进程杀掉（exit 143/124），
    // 表现为「停机挂起」的假阳性。runOnLoopStarted 早于 handler 安装，
    // 因此这里改用 loop 上的一次性定时器 + 显式 flush。
    drogon::app().getLoop()->runAfter(0.05, [] {
        std::cout << "READY" << std::endl;
        std::cout.flush();
    });

    // Drogon's production SIGTERM handler calls app().quit().  Once run()
    // returns, execute the same production shutdown sequence used by main.cc.
    drogon::app().run();
    lifecycle::runApplicationShutdown({
        [] { std::cout << "REAPER" << std::endl; },
        [] { std::cout << "ACCOUNTS" << std::endl; },
        [] { std::cout << "SESSION" << std::endl; },
        [] {
            BackgroundTaskQueue::instance().shutdown();
            std::cout << "QUEUE" << std::endl;
        },
    });
    std::cout << "EXIT" << std::endl;
    return 0;
}
