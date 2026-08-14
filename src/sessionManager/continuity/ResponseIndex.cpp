#include <sessionManager/continuity/ResponseIndex.h>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <vector>

namespace {
Json::StreamWriterBuilder& compactWriter()
{
    static Json::StreamWriterBuilder writer = [] {
        Json::StreamWriterBuilder value;
        value["indentation"] = "";
        return value;
    }();
    return writer;
}

bool parseResponseJson(const std::string& json, Json::Value& out)
{
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(json);
    return Json::parseFromStream(builder, stream, &out, &errors);
}

/// 数据库侧 TTL 使用绝对时间（epoch 秒）；内存侧仍用 steady_clock 判龄。
int64_t nowEpochSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

bool ResponseIndex::loadFromDbAndFill(const std::string& responseId, Entry& outEntry) {
    // 调用方需持有 mutex_；数据库异常在 SessionDbManager 内部已降级为未命中。
    if (!persistenceEnabled_.load()) return false;

    if (!persistence_) return false;
    auto row = persistence_->loadResponse(responseId);
    if (!row.has_value()) return false;

    Entry e;
    e.sessionId   = row->sessionId;
    e.createdAt   = std::chrono::steady_clock::now();
    e.hasResponse = row->hasResponse;
    if (row->hasResponse && !parseResponseJson(row->responseJson, e.response))
        e.hasResponse = false;

    map_[responseId] = e;
    outEntry = e;
    return true;
}

bool ResponseIndex::tryGetSessionId(const std::string& responseId, std::string& outSessionId) {
    if (responseId.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = map_.find(responseId);
    if (it != map_.end()) {
        outSessionId = it->second.sessionId;
        return !outSessionId.empty();
    }

    // 内存未命中：回查数据库并回填，支撑进程重启后的 previous_response_id 续接。
    Entry loaded;
    if (!loadFromDbAndFill(responseId, loaded)) return false;
    outSessionId = loaded.sessionId;
    return !outSessionId.empty();
}

void ResponseIndex::bind(const std::string& responseId, const std::string& sessionId) {
    if (responseId.empty()) return;

    std::vector<std::string> evicted;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& e = map_[responseId];
        e.sessionId = sessionId;
        if (e.createdAt == std::chrono::steady_clock::time_point{}) {
            e.createdAt = std::chrono::steady_clock::now();
        }

        // 防止无限增长：插入/更新时顺带清理
        evicted = cleanupLocked(kDefaultMaxEntries, kDefaultMaxAge);
    }

    if (!persistenceEnabled_.load()) return;

    // 写穿：hasResponse=false 表示只重绑 sessionId，不覆盖已有响应体。
    ResponsePersistenceRow row;
    row.responseId  = responseId;
    row.sessionId   = sessionId;
    row.hasResponse = false;
    row.createdAt   = nowEpochSeconds();
    if (persistence_) persistence_->asyncUpsertResponse(row);

    // 内存淘汰只降温，不删库：DB 行的生命周期跟随 chat_session_state 级联清理，
    // 以支撑内存已淘汰(6h)但会话快照仍在(24h)窗口内的 previous_response_id 续接。
    (void)evicted;
}

bool ResponseIndex::tryGetResponse(const std::string& responseId, std::string& outResponseJson) {
    if (responseId.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = map_.find(responseId);
    if (it != map_.end()) {
        if (!it->second.hasResponse) return false;
        outResponseJson = Json::writeString(compactWriter(), it->second.response);
        return true;
    }

    Entry loaded;
    if (!loadFromDbAndFill(responseId, loaded)) return false;
    if (!loaded.hasResponse) return false;
    outResponseJson = Json::writeString(compactWriter(), loaded.response);
    return true;
}

void ResponseIndex::storeResponse(const std::string& responseId, const std::string& responseJson) {
    if (responseId.empty()) return;
    Json::Value response;
    if (!parseResponseJson(responseJson, response)) return;

    std::vector<std::string> evicted;
    std::string sessionId;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& e = map_[responseId];
        if (e.createdAt == std::chrono::steady_clock::time_point{}) {
            e.createdAt = std::chrono::steady_clock::now();
        }
        e.hasResponse = true;
        e.response    = response;
        sessionId     = e.sessionId;

        evicted = cleanupLocked(kDefaultMaxEntries, kDefaultMaxAge);
    }

    if (!persistenceEnabled_.load()) return;

    ResponsePersistenceRow row;
    row.responseId  = responseId;
    row.sessionId   = sessionId;
    // 响应体默认不落库：内存热层仍保留完整响应，DB 只维持 responseId -> sessionId 映射。
    const bool persistBody = storeResponseBody_.load();
    row.hasResponse = persistBody;
    if (persistBody) row.responseJson = responseJson;
    row.createdAt   = nowEpochSeconds();
    if (persistence_) persistence_->asyncUpsertResponse(row);

    // 内存淘汰只降温，不删库：DB 行的生命周期跟随 chat_session_state 级联清理，
    // 以支撑内存已淘汰(6h)但会话快照仍在(24h)窗口内的 previous_response_id 续接。
    (void)evicted;
}

bool ResponseIndex::erase(const std::string& responseId) {
    if (responseId.empty()) return false;

    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        erased = map_.erase(responseId) > 0;
    }

    // 即便内存未命中也要删库：条目可能已被内存淘汰，但数据库行仍需清除，
    // 否则已删除的 responseId 会被懒加载"复活"。
    if (persistenceEnabled_.load()) {
        if (persistence_) persistence_->asyncDeleteResponses({responseId});
    }
    return erased;
}

void ResponseIndex::cleanup(size_t maxEntries, std::chrono::seconds maxAge) {
    // 仅做内存侧容量/时效控制。DB 侧不在此清理：response_index 是 chat_session_state
    // 的附属索引，其寿命由会话过期时级联删除决定，不再拥有独立的时间 TTL。
    std::lock_guard<std::mutex> lock(mutex_);
    (void)cleanupLocked(maxEntries, maxAge);
}

std::vector<std::string> ResponseIndex::cleanupLocked(size_t maxEntries,
                                                     std::chrono::seconds maxAge) {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> evicted;

    // 1) 先按 maxAge 清理过期的
    if (maxAge.count() > 0) {
        for (auto it = map_.begin(); it != map_.end();) {
            const auto age = now - it->second.createdAt;
            if (it->second.createdAt != std::chrono::steady_clock::time_point{} &&
                age > maxAge) {
                evicted.push_back(it->first);
                it = map_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 2) 再按 maxEntries 限制清理最老的
    if (maxEntries == 0 || map_.size() <= maxEntries) return evicted;

    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> items;
    items.reserve(map_.size());
    for (const auto& kv : map_) {
        items.emplace_back(kv.first, kv.second.createdAt);
    }

    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    const size_t toRemove = items.size() - maxEntries;
    for (size_t i = 0; i < toRemove; ++i) {
        evicted.push_back(items[i].first);
        map_.erase(items[i].first);
    }
    return evicted;
}
