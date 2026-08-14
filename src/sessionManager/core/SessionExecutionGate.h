#ifndef SESSION_EXECUTION_GATE_H
#define SESSION_EXECUTION_GATE_H

#include <string>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <functional>
#include <vector>

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

#include <domain/port/IExecutionGate.h>

namespace session {

/**
 * @brief 会话执行槽位
 * 
 * 表示一个会话的执行状态
 */
struct SessionSlot {
    // Protects both fields below.  They must change together: a stale guard
    // may finish after CancelPrevious has installed a successor.
    std::mutex mutex;
    CancellationSourcePtr currentCancellation;
    bool executing = false;
};

using SessionSlotPtr = std::shared_ptr<SessionSlot>;

/**
 * @brief 会话执行门控
 * 
 * 由 composition root 持有，管理所有会话的执行状态。
 */
class SessionExecutionGate final : public IExecutionGate {
public:
    SessionExecutionGate() = default;
    ~SessionExecutionGate() override = default;
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
        CancellationSourcePtr& outCancellation
    ) override {
        SessionSlotPtr slot = getOrCreateSlot(sessionKey);
        
        std::lock_guard<std::mutex> lock(slot->mutex);
        if (slot->executing) {
            if (policy == ConcurrencyPolicy::RejectConcurrent) {
                return GateResult::Rejected;
            }

            // CancelPrevious is intentionally cooperative: the new request
            // may start as soon as the old provider has been asked to stop.
            // The identity-aware release below prevents the old guard from
            // clearing this newly installed lease when it finally unwinds.
            if (slot->currentCancellation) {
                slot->currentCancellation->request();
            }
        }

        outCancellation = std::make_shared<platform::CancellationSource>();
        slot->currentCancellation = outCancellation;
        slot->executing = true;
        return GateResult::Acquired;
    }
    
    /**
     * @brief 释放执行权
     * 
     * @param sessionKey 会话标识
     */
    void release(const std::string& sessionKey,
                 const CancellationSourcePtr& cancellation) override {
        const auto slot = findSlot(sessionKey);
        if (!slot) return;

        std::lock_guard<std::mutex> lock(slot->mutex);
        if (slot->currentCancellation != cancellation) {
            // A CancelPrevious successor owns this key now.  The older guard
            // is allowed to finish, but must not release the newer lease.
            return;
        }
        slot->executing = false;
        slot->currentCancellation.reset();
    }
    
    /**
     * @brief 检查会话是否正在执行
     * 
     * @param sessionKey 会话标识
     * @return true 正在执行
     */
    bool isExecuting(const std::string& sessionKey) const override {
        const auto slot = findSlot(sessionKey);
        if (!slot) return false;
        std::lock_guard<std::mutex> lock(slot->mutex);
        return slot->executing;
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
            std::lock_guard<std::mutex> slotLock(pair.second->mutex);
            if (!pair.second->executing) {
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

    SessionSlotPtr findSlot(const std::string& sessionKey) const {
        std::lock_guard<std::mutex> lock(mapMutex_);
        const auto it = slots_.find(sessionKey);
        return it == slots_.end() ? nullptr : it->second;
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
        IExecutionGate& gate,
        const std::string& sessionKey,
        ConcurrencyPolicy policy = ConcurrencyPolicy::RejectConcurrent
    ) : gate_(gate), sessionKey_(sessionKey), acquired_(false) {
        result_ = gate_.tryAcquire(sessionKey, policy, cancellation_);
        acquired_ = (result_ == GateResult::Acquired);
    }
    
    ~ExecutionGuard() {
        if (acquired_) {
            gate_.release(sessionKey_, cancellation_);
        }
    }
    
    // 禁用拷贝
    ExecutionGuard(const ExecutionGuard&) = delete;
    ExecutionGuard& operator=(const ExecutionGuard&) = delete;
    
    // 允许移动
    ExecutionGuard(ExecutionGuard&& other) noexcept
        : gate_(other.gate_),
          sessionKey_(std::move(other.sessionKey_)),
          cancellation_(std::move(other.cancellation_)),
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
    
    /** Return the read-only token exposed to a provider call. */
    platform::CancellationToken cancellationToken() const
    {
        return cancellation_ ? cancellation_->token() : platform::CancellationToken{};
    }

    /** @brief 检查是否已被取消 */
    bool isCancelled() const
    {
        return cancellation_ && cancellation_->isCancelled();
    }
    
private:
    IExecutionGate& gate_;
    std::string sessionKey_;
    CancellationSourcePtr cancellation_;
    GateResult result_;
    bool acquired_;
};

} // 命名空间 会话

#endif // 头文件保护结束
