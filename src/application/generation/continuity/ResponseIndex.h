#ifndef RESPONSE_INDEX_H
#define RESPONSE_INDEX_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <json/json.h>
#include <domain/port/IResponseIndex.h>
#include <domain/port/ISessionPersistence.h>

/**
 * @brief ResponseIndex（内存热层 + 数据库持久层）
 *
 * 职责：
 * - /v1/responses: 维护 responseId -> sessionId 的映射（用于 previous_response_id 续接）
 * - 可选：存储 responseId -> response JSON（用于 GET /responses/{id}）
 *
 * 持久化设计（写穿 + 懒加载）：
 * - 内存 map_ 仍是唯一热路径来源，读命中时不触碰数据库。
 * - bind()/storeResponse() 更新内存后，异步写穿到 response_index 表。
 * - 内存未命中时回查数据库并回填内存，使进程重启后仍可按
 *   previous_response_id 续接会话。
 * - erase() 与 cleanup() 淘汰的条目会同步删库，避免「内存已淘汰、
 *   数据库仍可复活」的不一致。
 * - 任何数据库异常都静默降级为未命中，退回「按新会话处理」，不阻塞请求链路。
 * - 持久化默认关闭，由启动流程在建表成功后开启；关闭时行为与旧版纯内存实现一致。
 * - 仍保留基于 maxEntries/maxAge 的清理策略，防止内存无限增长。
 */
class ResponseIndex final : public IResponseIndex {
public:
    explicit ResponseIndex(ISessionPersistence* persistence = nullptr)
        : persistence_(persistence) {}
    ~ResponseIndex() override = default;

    // 映射：responseId -> sessionId
    bool tryGetSessionId(const std::string& responseId, std::string& outSessionId) override;
    void bind(const std::string& responseId, const std::string& sessionId) override;

    // 映射：responseId -> 响应 JSON（可选存储）
    bool tryGetResponse(const std::string& responseId, std::string& outResponseJson) override;
    void storeResponse(const std::string& responseId, const std::string& responseJson) override;
    bool erase(const std::string& responseId) override;


    void cleanup(size_t maxEntries, std::chrono::seconds maxAge) override;

    /// 开关写穿/懒加载；由启动流程在 SessionDbManager 建表成功后开启。
    void setPersistenceEnabled(bool enabled) { persistenceEnabled_ = enabled; }
    bool isPersistenceEnabled() const { return persistenceEnabled_.load(); }

    // 响应体是否落库。默认关闭：response_body 仅供 GET /responses/{id} 回放，
    // 不参与 previous_response_id 续接，关闭后只写 response_id -> session_id 映射。
    void setStoreResponseBody(bool enabled) { storeResponseBody_ = enabled; }
    bool isStoreResponseBodyEnabled() const { return storeResponseBody_.load(); }

    static constexpr size_t kDefaultMaxEntries = 200000;
    static constexpr std::chrono::seconds kDefaultMaxAge = std::chrono::hours(6);

private:
    ResponseIndex(const ResponseIndex&) = delete;
    ResponseIndex& operator=(const ResponseIndex&) = delete;

    struct Entry {
        std::string sessionId;
        std::chrono::steady_clock::time_point createdAt;
        bool hasResponse = false;
        Json::Value response;
    };

    /// 返回本次被淘汰的 responseId，供调用方在解锁后同步删库。
    std::vector<std::string> cleanupLocked(size_t maxEntries, std::chrono::seconds maxAge);

    /// 内存未命中时从数据库回查并回填内存；返回是否命中。
    bool loadFromDbAndFill(const std::string& responseId, Entry& outEntry);

    std::mutex mutex_;
    std::unordered_map<std::string, Entry> map_;
    std::atomic<bool> persistenceEnabled_{false};
    std::atomic<bool> storeResponseBody_{false};
    ISessionPersistence* persistence_ = nullptr;
};

#endif // 头文件保护结束
