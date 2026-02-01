#ifndef ERROR_STATS_CONFIG_H
#define ERROR_STATS_CONFIG_H

#include <string>
#include <cstdint>
#include <json/json.h>

namespace metrics {

/**
 * @brief 队列满时的丢弃策略
 */
enum class DropPolicy {
    DROP_OLDEST,  // 丢弃最旧的事件
    DROP_NEWEST   // 丢弃最新的事件（即当前要入队的）
};

/**
 * @brief 错误统计配置结构体
 * 
 * 对应 config.json 中的 custom_config.error_stats 配置项
 */
struct ErrorStatsConfig {
    // ========== 功能开关 ==========
    bool enabled = true;              // 是否启用错误统计
    bool persistDetail = true;        // 是否持久化明细到 error_event 表
    bool persistAgg = true;           // 是否持久化聚合到 error_agg_hour 表
    bool persistRequestAgg = true;    // 是否持久化请求聚合到 request_agg_hour 表
    
    // ========== 保留策略 ==========
    int retentionDaysDetail = 30;     // 明细保留天数
    int retentionDaysAgg = 30;        // 错误聚合保留天数
    int retentionDaysRequestAgg = 30; // 请求聚合保留天数
    
    // ========== raw_snippet 配置 ==========
    bool rawSnippetEnabled = true;    // 是否存储 raw_snippet
    size_t rawSnippetMaxLen = 32768;  // raw_snippet 最大长度（32KB）
    
    // ========== 异步写入配置 ==========
    size_t asyncBatchSize = 200;      // 批量写入大小
    size_t asyncFlushMs = 200;        // 刷新间隔（毫秒）
    DropPolicy dropPolicy = DropPolicy::DROP_OLDEST;  // 队列满时的丢弃策略
    size_t queueCapacity = 10000;     // 队列最大容量
    
    /**
     * @brief 从 Json::Value 加载配置
     * 
     * @param config custom_config.error_stats 节点
     * @return ErrorStatsConfig 配置对象
     */
    static ErrorStatsConfig loadFromJson(const Json::Value& config);
    
    /**
     * @brief 获取全局配置实例（单例）
     * 
     * @return ErrorStatsConfig& 配置引用
     */
    static ErrorStatsConfig& getInstance();
    
    /**
     * @brief 初始化全局配置（从 drogon app 配置加载）
     * 
     * 应在 app 启动时调用一次
     */
    static void initFromApp();
    
    /**
     * @brief 将 DropPolicy 枚举转换为字符串
     */
    static std::string dropPolicyToString(DropPolicy p) {
        switch (p) {
            case DropPolicy::DROP_OLDEST: return "drop_oldest";
            case DropPolicy::DROP_NEWEST: return "drop_newest";
            default: return "drop_oldest";
        }
    }
    
    /**
     * @brief 将字符串转换为 DropPolicy 枚举
     */
    static DropPolicy stringToDropPolicy(const std::string& s) {
        if (s == "drop_newest") return DropPolicy::DROP_NEWEST;
        return DropPolicy::DROP_OLDEST;  // 默认 drop_oldest
    }
};

} // namespace metrics

#endif // ERROR_STATS_CONFIG_H
