#pragma once

#include <drogon/HttpResponse.h>
#include <trantor/net/EventLoop.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

/**
 * Executes ResponseStream operations on the event loop that owns the
 * underlying TCP connection.
 *
 * Calling Trantor's async stream from a worker thread has a check-then-queue
 * window: the peer may disconnect after the connection-state check but before
 * the queued stream operation runs. Keeping send, close, and destruction on
 * the owning loop removes that race.
 */
class IoLoopResponseStream final
    : public std::enable_shared_from_this<IoLoopResponseStream>
{
public:
    using Ptr = std::shared_ptr<IoLoopResponseStream>;

    static Ptr create(drogon::ResponseStreamPtr stream,
                      trantor::EventLoop *loop = nullptr)
    {
        if (!stream)
        {
            return {};
        }
        if (loop == nullptr)
        {
            loop = trantor::EventLoop::getEventLoopOfCurrentThread();
        }
        if (loop == nullptr)
        {
            return {};
        }
        return Ptr(new IoLoopResponseStream(std::move(stream), loop));
    }

    bool send(std::string chunk)
    {
        if (closeRequested_.load(std::memory_order_acquire))
        {
            return false;
        }

        auto self = shared_from_this();
        auto task = [self, chunk = std::move(chunk)]() mutable {
            self->sendInLoop(chunk);
        };
        if (loop_->isInLoopThread())
        {
            task();
        }
        else
        {
            loop_->queueInLoop(std::move(task));
        }
        return !closeRequested_.load(std::memory_order_acquire);
    }

    void close()
    {
        bool expected = false;
        if (!closeRequested_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return;
        }

        auto self = shared_from_this();
        if (loop_->isInLoopThread())
        {
            self->closeInLoop();
        }
        else
        {
            loop_->queueInLoop([self]() { self->closeInLoop(); });
        }
    }

    bool isOpen() const noexcept
    {
        return !closeRequested_.load(std::memory_order_acquire);
    }

private:
    struct LoopBoundDeleter
    {
        trantor::EventLoop *loop = nullptr;

        void operator()(drogon::ResponseStream *stream) const noexcept
        {
            if (stream == nullptr)
            {
                return;
            }
            if (loop == nullptr || loop->isInLoopThread() || !loop->isRunning())
            {
                delete stream;
                return;
            }
            loop->queueInLoop([stream]() { delete stream; });
        }
    };

    IoLoopResponseStream(drogon::ResponseStreamPtr stream,
                         trantor::EventLoop *loop)
        : loop_(loop),
          stream_(stream.release(), LoopBoundDeleter{loop})
    {
    }

    void sendInLoop(const std::string &chunk)
    {
        if (closed_.load(std::memory_order_acquire) || !stream_)
        {
            return;
        }
        if (!stream_->send(chunk))
        {
            closeRequested_.store(true, std::memory_order_release);
            closeInLoop();
        }
    }

    void closeInLoop()
    {
        if (closed_.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        auto stream = std::move(stream_);
        if (stream)
        {
            stream->close();
        }
    }

    trantor::EventLoop *loop_ = nullptr;
    std::shared_ptr<drogon::ResponseStream> stream_;
    std::atomic<bool> closeRequested_{false};
    std::atomic<bool> closed_{false};
};
