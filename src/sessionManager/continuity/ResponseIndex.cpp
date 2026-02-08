#include "sessionManager/continuity/ResponseIndex.h"
#include <algorithm>
#include <vector>

ResponseIndex& ResponseIndex::instance() {
    static ResponseIndex inst;
    return inst;
}

bool ResponseIndex::tryGetSessionId(const std::string& responseId, std::string& outSessionId) {
    if (responseId.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(responseId);
    if (it == map_.end()) return false;
    outSessionId = it->second.sessionId;
    return !outSessionId.empty();
}

void ResponseIndex::bind(const std::string& responseId, const std::string& sessionId) {
    if (responseId.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);

    auto& e = map_[responseId];
    e.sessionId = sessionId;
    if (e.createdAt == std::chrono::steady_clock::time_point{}) {
        e.createdAt = std::chrono::steady_clock::now();
    }

    // 防止无限增长：插入/更新时顺带清理
    cleanupLocked(kDefaultMaxEntries, kDefaultMaxAge);
}

bool ResponseIndex::tryGetResponse(const std::string& responseId, Json::Value& outResponse) {
    if (responseId.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(responseId);
    if (it == map_.end()) return false;
    if (!it->second.hasResponse) return false;
    outResponse = it->second.response;
    return true;
}

void ResponseIndex::storeResponse(const std::string& responseId, const Json::Value& response) {
    if (responseId.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);

    auto& e = map_[responseId];
    if (e.createdAt == std::chrono::steady_clock::time_point{}) {
        e.createdAt = std::chrono::steady_clock::now();
    }
    e.hasResponse = true;
    e.response = response;

    cleanupLocked(kDefaultMaxEntries, kDefaultMaxAge);
}

bool ResponseIndex::erase(const std::string& responseId) {
    if (responseId.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.erase(responseId) > 0;
}

void ResponseIndex::cleanup(size_t maxEntries, std::chrono::seconds maxAge) {
    std::lock_guard<std::mutex> lock(mutex_);
    cleanupLocked(maxEntries, maxAge);
}

void ResponseIndex::cleanupLocked(size_t maxEntries, std::chrono::seconds maxAge) {
    const auto now = std::chrono::steady_clock::now();


    if (maxAge.count() > 0) {
        for (auto it = map_.begin(); it != map_.end();) {
            const auto age = now - it->second.createdAt;
            if (it->second.createdAt != std::chrono::steady_clock::time_point{} &&
                age > maxAge) {
                it = map_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 2) 再按 限制清理最老的
    if (maxEntries == 0 || map_.size() <= maxEntries) return;

    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> items;
    items.reserve(map_.size());
    for (const auto& kv : map_) {
        items.emplace_back(kv.first, kv.second.createdAt);
    }

    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    const size_t toRemove = items.size() - maxEntries;
    for (size_t i = 0; i < toRemove; ++i) {
        map_.erase(items[i].first);
    }
}

