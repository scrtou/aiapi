#include <drogon/drogon.h>

#include <utils/ApplicationShutdown.h>
#include <utils/BackgroundTaskQueue.h>

#include <iostream>

int main()
{
    BackgroundTaskQueue::instance().start(2);
    drogon::app().getLoop()->queueInLoop([] {
        std::cout << "READY" << std::endl;
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
