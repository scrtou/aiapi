#pragma once

#include <chrono>
#include <drogon/drogon.h>
#include <drogon/HttpFilter.h>
#include <mutex>
#include <string>
#include <unordered_map>

class RateLimitFilter : public drogon::HttpFilter<RateLimitFilter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req,
                  drogon::FilterCallback&& fcb,
                  drogon::FilterChainCallback&& fccb) override {
        const auto& customConfig = drogon::app().getCustomConfig();
        const auto& rateLimit = customConfig["rate_limit"];

        if (!rateLimit.isObject() || !rateLimit.get("enabled", false).asBool()) {
            fccb();
            return;
        }

        const int requestsPerSecond = rateLimit.get("requests_per_second", 10).asInt();
        const int burst = rateLimit.get("burst", 20).asInt();
        if (requestsPerSecond <= 0 || burst <= 0) {
            fccb();
            return;
        }

        const std::string key = req->peerAddr().toIp();
        const auto now = std::chrono::steady_clock::now();

        bool allowed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& bucket = buckets_[key];
            if (bucket.lastRefill.time_since_epoch().count() == 0) {
                bucket.tokens = static_cast<double>(burst);
                bucket.lastRefill = now;
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - bucket.lastRefill).count();
            bucket.tokens = std::min<double>(burst, bucket.tokens + elapsed * requestsPerSecond);
            bucket.lastRefill = now;

            if (bucket.tokens >= 1.0) {
                bucket.tokens -= 1.0;
                allowed = true;
            }
        }

        if (allowed) {
            fccb();
            return;
        }

        Json::Value error;
        error["error"]["message"] = "Rate limit exceeded";
        error["error"]["type"] = "rate_limit_error";
        error["error"]["code"] = "too_many_requests";

        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k429TooManyRequests);
        resp->addHeader("Retry-After", "1");
        fcb(resp);
    }

private:
    struct TokenBucket {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point lastRefill;
    };

    std::mutex mutex_;
    std::unordered_map<std::string, TokenBucket> buckets_;
};

