#pragma once

#include <application/generation/protocol/claude/ClaudeErrorFormatter.h>

#include <drogon/HttpFilter.h>
#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

/** Rate limiting for Anthropic routes with an Anthropic-shaped error body. */
class ClaudeRateLimitFilter : public drogon::HttpFilter<ClaudeRateLimitFilter>
{
  public:
    void doFilter(const drogon::HttpRequestPtr& req,
                  drogon::FilterCallback&& reject,
                  drogon::FilterChainCallback&& next) override
    {
        const auto& rateLimit = drogon::app().getCustomConfig()["rate_limit"];
        if (!rateLimit.isObject() || !rateLimit.get("enabled", false).asBool()) {
            next();
            return;
        }

        const int requestsPerSecond = rateLimit.get("requests_per_second", 10).asInt();
        const int burst = rateLimit.get("burst", 20).asInt();
        if (requestsPerSecond <= 0 || burst <= 0) {
            next();
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        bool allowed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& bucket = buckets_[req->peerAddr().toIp()];
            if (bucket.lastRefill.time_since_epoch().count() == 0) {
                bucket.tokens = static_cast<double>(burst);
                bucket.lastRefill = now;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
                now - bucket.lastRefill).count();
            bucket.tokens = std::min<double>(
                burst, bucket.tokens + elapsed * requestsPerSecond);
            bucket.lastRefill = now;
            if (bucket.tokens >= 1.0) {
                bucket.tokens -= 1.0;
                allowed = true;
            }
        }

        if (allowed) {
            next();
            return;
        }

        const auto body = generation::protocol::claude::formatRateLimitError();
        auto response = drogon::HttpResponse::newHttpJsonResponse(body);
        response->setStatusCode(drogon::k429TooManyRequests);
        response->addHeader("Retry-After", "1");
        reject(response);
    }

  private:
    struct TokenBucket {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point lastRefill;
    };

    std::mutex mutex_;
    std::unordered_map<std::string, TokenBucket> buckets_;
};
