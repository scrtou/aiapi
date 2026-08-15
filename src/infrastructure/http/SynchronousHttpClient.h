#pragma once

#include <drogon/drogon.h>

#include <cstddef>
#include <string>

namespace infrastructure::http {

/**
 * Select an already-running Drogon loop that is not the caller's loop.
 *
 * Drogon's synchronous HttpClient API waits for a callback that is delivered
 * by the client's event loop.  Constructing a client without an explicit
 * loop binds it to app().getLoop() (the main loop).  Startup wiring runs in a
 * BeginningAdvice on exactly that loop, so the default client trips Drogon's
 * deadlock assertion before it can issue an upstream request.
 *
 * Prefer an I/O loop.  When a synchronous request originates on one of the
 * I/O loops, select another I/O loop if possible and fall back to the running
 * main loop.  Returning nullptr is intentional when Drogon has not started
 * its I/O loops yet or when every available loop is the caller's loop: using
 * the default loop in either case would deadlock instead of failing safely.
 */
inline trantor::EventLoop* selectSynchronousHttpClientLoop()
{
    auto& app = drogon::app();

    const auto ioLoopCount = app.getThreadNum();
    if (ioLoopCount > 0) {
        // getIOLoop() is only valid after app().run() has created the I/O
        // pool.  A null result is therefore a deliberate "not ready" signal,
        // not a reason to fall back to the not-yet-running main loop.
        auto* firstIoLoop = app.getIOLoop(0);
        if (firstIoLoop == nullptr) {
            return nullptr;
        }

        if (firstIoLoop->isRunning() && !firstIoLoop->isInLoopThread()) {
            return firstIoLoop;
        }

        for (std::size_t index = 1; index < ioLoopCount; ++index) {
            auto* const loop = app.getIOLoop(index);
            if (loop != nullptr && loop->isRunning() && !loop->isInLoopThread()) {
                return loop;
            }
        }
    }

    auto* const mainLoop = app.getLoop();
    if (mainLoop != nullptr && mainLoop->isRunning() && !mainLoop->isInLoopThread()) {
        return mainLoop;
    }
    return nullptr;
}

/** Create a Drogon client that is safe for the synchronous sendRequest API. */
inline drogon::HttpClientPtr makeSynchronousHttpClient(const std::string& baseUrl)
{
    auto* const loop = selectSynchronousHttpClientLoop();
    if (loop == nullptr) {
        LOG_ERROR << "[HTTP] 同步上游请求没有可用的非当前 Drogon event loop";
        return nullptr;
    }
    return drogon::HttpClient::newHttpClient(baseUrl, loop);
}

}  // namespace infrastructure::http
