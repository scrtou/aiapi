#ifndef RESPONSE_INDEX_H
#define RESPONSE_INDEX_H

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <json/json.h>

/**
 * @brief ResponseIndex（纯内存）
 *
 * 职责：
 * - /v1/responses: 维护 responseId -> sessionId 的映射（用于 previous_response_id 续接）
 * - 可选：存储 responseId -> response JSON（用于 GET /responses/{id}）
 *
 * 说明：
 * - 该索引是纯内存结构，允许重启丢失；丢失后按设计降级为新会话。
 * - 提供基于 maxEntries/maxAge 的清理策略，防止内存无限增长。
 */
class ResponseIndex {
public:
    static ResponseIndex& instance();

    // --- mapping: responseId -> sessionId ---
    bool tryGetSessionId(const std::string& responseId, std::string& outSessionId);
    void bind(const std::string& responseId, const std::string& sessionId);

    // --- storage: responseId -> response JSON (optional) ---
    bool tryGetResponse(const std::string& responseId, Json::Value& outResponse);
    void storeResponse(const std::string& responseId, const Json::Value& response);
    bool erase(const std::string& responseId);

    // --- housekeeping ---
    void cleanup(size_t maxEntries, std::chrono::seconds maxAge);

    static constexpr size_t kDefaultMaxEntries = 200000;
    static constexpr std::chrono::seconds kDefaultMaxAge = std::chrono::hours(6);

private:
    ResponseIndex() = default;
    ~ResponseIndex() = default;

    ResponseIndex(const ResponseIndex&) = delete;
    ResponseIndex& operator=(const ResponseIndex&) = delete;

    struct Entry {
        std::string sessionId;
        std::chrono::steady_clock::time_point createdAt;
        bool hasResponse = false;
        Json::Value response;
    };

    void cleanupLocked(size_t maxEntries, std::chrono::seconds maxAge);

    std::mutex mutex_;
    std::unordered_map<std::string, Entry> map_;
};

#endif // RESPONSE_INDEX_H

