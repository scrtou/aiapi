#ifndef SESSION_EXECUTION_GATE_H
#define SESSION_EXECUTION_GATE_H

#include <string>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <functional>

/**
 * @brief 会话执行门控
 * 
 * 用于控制同一会话的并发执行，防止同一 sessionKey 的请求并发执行导致输出互相打架。
 * 
 * 策略：
 * - RejectConcurrent: 拒绝并发请求（返回 409 Conflict）
 * - CancelPrevious: 取消之前的请求，执行新请求
 * 
 * 参考设计文档: plans/aiapi-refactor-design.md 第 9 节
 */

namespace session {

/**
 * @brief 取消令牌
 * 
 * 用于请求级取消标记。请求执行过程中可以检查是否已被取消。
 */
class CancellationToken {
public:
    CancellationToken() : cancelled_(false) {}
    
    /**
     * @brief 请求取消
     */
    void cancel() {
        cancelled_.store(true, std::memory_order_release);
    }
    
    /**
     * @brief 检查是否已取消
     */
    bool isCancelled() const {
        return cancelled_.load(std::memory_order_acquire);
    }
    
    /**
     * @brief 重置取消状态
     */
    void reset() {
        cancelled_.store(false, std::memory_order_release);
    }
    
private:
    std::atomic<bool> cancelled_;
};

using CancellationTokenPtr = std::shared_ptr<CancellationToken>;

/**
 * @brief 并发策略
 */
enum class ConcurrencyPolicy {
    RejectConcurrent,   // 拒绝并发请求
    CancelPrevious      // 取消之前的请求
};

/**
 * @brief 执行门控结果
 */
enum class GateResult {
    Acquired,           // 成功获取执行权
    Rejected,           // 被拒绝（已有请求在执行）
    Cancelled           // 之前的请求被取消
};

/**
 * @brief 会话执行槽位
 * 
 * 表示一个会话的执行状态
 */
struct SessionSlot {
    std::mutex mutex;                   // 执行互斥锁
    CancellationTokenPtr currentToken;  // 当前执行的取消令牌
    std::atomic<bool> executing{false}; // 是否正在执行
    
    SessionSlot() : currentToken(nullptr) {}
};

using SessionSlotPtr = std::shared_ptr<SessionSlot>;

/**
 * @brief 会话执行门控
 * 
 * 单例模式，管理所有会话的执行状态
 */
class SessionExecutionGate {
public:
    /**
     * @brief 获取单例实例
     */
    static SessionExecutionGate& getInstance() {
        static SessionExecutionGate instance;
        return instance;
    }
    
    /**
     * @brief 尝试获取执行权
     * 
     * @param sessionKey 会话标识
     * @param policy 并发策略
     * @param outToken 输出参数，成功获取时返回取消令牌
     * @return GateResult 获取结果
     */
    GateResult tryAcquire(
        const std::string& sessionKey,
        ConcurrencyPolicy policy,
        CancellationTokenPtr& outToken
    ) {
        SessionSlotPtr slot = getOrCreateSlot(sessionKey);
        
        // 尝试获取槽位锁
        std::unique_lock<std::mutex> lock(slot->mutex, std::try_to_lock);
        
        if (!lock.owns_lock()) {
            // 无法立即获取锁，说明有请求正在执行
            if (policy == ConcurrencyPolicy::RejectConcurrent) {
                return GateResult::Rejected;
            }
            
            // CancelPrevious 策略：取消之前的请求
            if (slot->currentToken) {
                slot->currentToken->cancel();
            }
            
            // 等待获取锁
            lock.lock();
        }
        
        // 成功获取执行权
        outToken = std::make_shared<CancellationToken>();
        slot->currentToken = outToken;
        slot->executing.store(true, std::memory_order_release);
        
        return GateResult::Acquired;
    }
    
    /**
     * @brief 释放执行权
     * 
     * @param sessionKey 会话标识
     */
    void release(const std::string& sessionKey) {
        std::lock_guard<std::mutex> mapLock(mapMutex_);
        auto it = slots_.find(sessionKey);
        if (it != slots_.end()) {
            it->second->executing.store(false, std::memory_order_release);
            it->second->currentToken = nullptr;
        }
    }
    
    /**
     * @brief 检查会话是否正在执行
     * 
     * @param sessionKey 会话标识
     * @return true 正在执行
     */
    bool isExecuting(const std::string& sessionKey) const {
        std::lock_guard<std::mutex> mapLock(mapMutex_);
        auto it = slots_.find(sessionKey);
        if (it != slots_.end()) {
            return it->second->executing.load(std::memory_order_acquire);
        }
        return false;
    }
    
    /**
     * @brief 清理过期的槽位（可选，用于内存管理）
     * 
     * @param maxIdleSlots 最大空闲槽位数
     */
    void cleanup(size_t maxIdleSlots = 1000) {
        std::lock_guard<std::mutex> mapLock(mapMutex_);
        
        // 移除未在执行的槽位，保留最多 maxIdleSlots 个
        std::vector<std::string> toRemove;
        for (const auto& pair : slots_) {
            if (!pair.second->executing.load(std::memory_order_acquire)) {
                toRemove.push_back(pair.first);
            }
        }
        
        // 如果空闲槽位超过限制，移除多余的
        if (toRemove.size() > maxIdleSlots) {
            for (size_t i = maxIdleSlots; i < toRemove.size(); ++i) {
                slots_.erase(toRemove[i]);
            }
        }
    }
    
private:
    SessionExecutionGate() = default;
    ~SessionExecutionGate() = default;
    
    // 禁用拷贝
    SessionExecutionGate(const SessionExecutionGate&) = delete;
    SessionExecutionGate& operator=(const SessionExecutionGate&) = delete;
    
    /**
     * @brief 获取或创建会话槽位
     */
    SessionSlotPtr getOrCreateSlot(const std::string& sessionKey) {
        std::lock_guard<std::mutex> lock(mapMutex_);
        auto it = slots_.find(sessionKey);
        if (it == slots_.end()) {
            auto slot = std::make_shared<SessionSlot>();
            slots_[sessionKey] = slot;
            return slot;
        }
        return it->second;
    }
    
    mutable std::mutex mapMutex_;
    std::unordered_map<std::string, SessionSlotPtr> slots_;
};

/**
 * @brief RAII 风格的执行门控守卫
 * 
 * 自动在作用域结束时释放执行权
 */
class ExecutionGuard {
public:
    ExecutionGuard(
        const std::string& sessionKey,
        ConcurrencyPolicy policy = ConcurrencyPolicy::RejectConcurrent
    ) : sessionKey_(sessionKey), acquired_(false) {
        result_ = SessionExecutionGate::getInstance().tryAcquire(
            sessionKey, policy, token_
        );
        acquired_ = (result_ == GateResult::Acquired);
    }
    
    ~ExecutionGuard() {
        if (acquired_) {
            SessionExecutionGate::getInstance().release(sessionKey_);
        }
    }
    
    // 禁用拷贝
    ExecutionGuard(const ExecutionGuard&) = delete;
    ExecutionGuard& operator=(const ExecutionGuard&) = delete;
    
    // 允许移动
    ExecutionGuard(ExecutionGuard&& other) noexcept
        : sessionKey_(std::move(other.sessionKey_)),
          token_(std::move(other.token_)),
          result_(other.result_),
          acquired_(other.acquired_) {
        other.acquired_ = false;
    }
    
    /**
     * @brief 是否成功获取执行权
     */
    bool isAcquired() const { return acquired_; }
    
    /**
     * @brief 获取门控结果
     */
    GateResult getResult() const { return result_; }
    
    /**
     * @brief 获取取消令牌
     */
    CancellationTokenPtr getToken() const { return token_; }
    
    /**
     * @brief 检查是否已被取消
     */
    bool isCancelled() const {
        return token_ && token_->isCancelled();
    }
    
private:
    std::string sessionKey_;
    CancellationTokenPtr token_;
    GateResult result_;
    bool acquired_;
};

} // 命名空间 会话

#endif // 头文件保护结束
