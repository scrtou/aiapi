#ifndef IRESPONSE_SINK_H
#define IRESPONSE_SINK_H

#include "sessionManager/contracts/GenerationEvent.h"
/**
 * @brief 输出通道接口
 * 
 * Session/UseCase 产生语义事件，通过 IResponseSink 接口输出。
 * Controller 侧提供不同协议的 Sink 实现（Chat SSE / Chat JSON / Responses SSE / Responses JSON）。
 * 
 * 这样 Session 层不需要知道具体的输出协议细节。
 * 
 * 参考设计文档: plans/aiapi-refactor-design.md 第 4.3 节
 */
class IResponseSink {
public:
    virtual ~IResponseSink() = default;
    
    /**
     * @brief 接收生成事件
     * 
     * @param event 生成事件（Started/OutputTextDelta/OutputTextDone/Usage/Completed/Error）
     */
    virtual void onEvent(const generation::GenerationEvent& event) = 0;
    
    /**
     * @brief 关闭输出通道
     * 
     * 在所有事件发送完毕后调用，用于清理资源和关闭连接
     */
    virtual void onClose() = 0;
    
    /**
     * @brief 检查输出通道是否仍然有效
     * 
     * @return true 如果通道仍可用于发送事件
     */
    virtual bool isValid() const { return true; }
    
    /**
     * @brief 获取 Sink 类型名称（用于日志）
     * 
     * @return Sink 类型的描述字符串
     */
    virtual std::string getSinkType() const = 0;
};

/**
 * @brief 空 Sink 实现
 * 
 * 用于测试或丢弃输出的场景
 */
class NullSink : public IResponseSink {
public:
    void onEvent(const generation::GenerationEvent& /*忽略事件参数*/) override {}
    void onClose() override {}
    std::string getSinkType() const override { return "NullSink"; }
};

/**
 * @brief 收集器 Sink 实现
 * 
 * 收集所有事件，用于测试或需要完整结果的场景
 */
#include <vector>

class CollectorSink : public IResponseSink {
public:
    void onEvent(const generation::GenerationEvent& event) override {
        events_.push_back(event);
    }
    
    void onClose() override {
        closed_ = true;
    }
    
    std::string getSinkType() const override { return "CollectorSink"; }
    
    const std::vector<generation::GenerationEvent>& getEvents() const {
        return events_;
    }
    
    bool isClosed() const {
        return closed_;
    }
    
    /**
     * @brief 获取最终的完整文本
     * 
     * 从 OutputTextDone 事件中提取
     */
    std::string getFinalText() const {
        for (const auto& event : events_) {
            if (std::holds_alternative<generation::OutputTextDone>(event)) {
                return std::get<generation::OutputTextDone>(event).text;
            }
        }
        return "";
    }
    
    /**
     * @brief 检查是否有错误
     */
    bool hasError() const {
        for (const auto& event : events_) {
            if (std::holds_alternative<generation::Error>(event)) {
                return true;
            }
        }
        return false;
    }
    
    /**
     * @brief 获取错误事件（如果有）
     */
    std::optional<generation::Error> getError() const {
        for (const auto& event : events_) {
            if (std::holds_alternative<generation::Error>(event)) {
                return std::get<generation::Error>(event);
            }
        }
        return std::nullopt;
    }
    
private:
    std::vector<generation::GenerationEvent> events_;
    bool closed_ = false;
};

#endif // 头文件保护结束
