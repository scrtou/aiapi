#pragma once

#include <functional>

namespace lifecycle {

/**
 * The four callbacks are the shutdown ownership boundary currently used by
 * main.cc.  Keeping the ordering in one production function makes the
 * post-SIGTERM path observable without booting databases or real providers.
 */
struct ApplicationShutdownActions
{
    std::function<void()> stopReaper;
    std::function<void()> stopAccountWorkers;
    std::function<void()> stopSessionCleaner;
    std::function<void()> shutdownTaskQueue;
};

void runApplicationShutdown(const ApplicationShutdownActions& actions);

}  // namespace lifecycle
