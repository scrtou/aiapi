#ifndef STATUS_DB_MANAGER_H
#define STATUS_DB_MANAGER_H

#include <drogon/drogon.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>
#include <domain/model/MetricsData.h>
#include <domain/port/IMetricsQuery.h>

namespace metrics {

/**
 * @brief 服务状态数据库管理器
 *
 * 负责从 request_agg_hour 和 error_agg_hour 表查询数据，
 * 计算渠道和模型的健康状态
 */
class StatusDbManager : public IStatusMetricsQuery {
public:
    /**
     * @brief 初始化
     */
    void init();
    
    /**
     * @brief 获取服务状态概览
     */
    StatusSummaryData getStatusSummary(const StatusQueryParams& params) override;
    
    /**
     * @brief 获取渠道状态列表
     */
    std::vector<ChannelStatusData> getChannelStatusList(const StatusQueryParams& params) override;
    
    /**
     * @brief 获取模型状态列表（按 model×provider 维度）
     */
    std::vector<ModelStatusData> getModelStatusList(const StatusQueryParams& params) override;
    
    /**
     * @brief 将健康状态枚举转换为字符串
     */
    static std::string statusToString(ServiceHealthStatus status);
    
    /**
     * @brief 根据错误率计算健康状态
     * @param errorRate 错误率 (0.0 - 1.0)
     * @param requestCount 请求数（用于判断是否有足够数据）
     */
    static ServiceHealthStatus calculateStatus(double errorRate, int64_t requestCount);

private:
    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    bool isPostgres_ = true;
    
    /**
     * @brief 检测数据库类型
     */
    void detectDbType();
    
    /**
     * @brief 确保数据库客户端已初始化（延迟初始化）
     */
    void ensureDbClient();
    
    /**
     * @brief 格式化时间为 UTC 字符串
     */
    std::string formatTimeUtc(std::chrono::system_clock::time_point tp);
    
    /**
     * @brief 获取默认时间范围（最近 24 小时）
     */
    std::pair<std::string, std::string> getDefaultTimeRange();
    
    /**
     * @brief 一次性获取所有渠道的时间序列数据（优化：避免 N+1 查询）
     * @param from 开始时间
     * @param to 结束时间
     * @return map<provider, buckets>
     */
    std::map<std::string, std::vector<StatusBucket>> fetchAllChannelBuckets(
        const std::string& from, const std::string& to);
    
    /**
     * @brief 一次性获取所有模型的时间序列数据（优化：避免 N+1 查询）
     * @param from 开始时间
     * @param to 结束时间
     * @return map<model:provider, buckets>
     */
    std::map<std::string, std::vector<StatusBucket>> fetchAllModelBuckets(
        const std::string& from, const std::string& to);
    
    /**
     * @brief 轻量级获取渠道状态统计（不获取时间序列数据）
     * @param from 开始时间
     * @param to 结束时间
     */
    ChannelStatusCounts getChannelStatusCounts(const std::string& from, const std::string& to);
};

} // 命名空间结束

#endif // 头文件保护结束
