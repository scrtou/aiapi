#ifndef ERROR_STATS_DB_MANAGER_H
#define ERROR_STATS_DB_MANAGER_H

#include <drogon/drogon.h>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <memory>
#include <chrono>
#include <domain/model/ErrorEvent.h>
#include <domain/model/MetricsData.h>
#include <domain/model/RequestAggData.h>
#include <domain/port/IErrorStatsSink.h>
#include <domain/port/IMetricsQuery.h>

namespace metrics {

/**
 * @brief 数据库类型枚举
 */
enum class DbType {
    PostgreSQL,
    SQLite3,
    MySQL
};

/**
 * @brief 错误统计数据库管理器
 *
 * 负责 error_event、error_agg_hour、request_agg_hour 三张表的
 * 创建、升级、批量写入、聚合 upsert、查询等操作
 */
class ErrorStatsDbManager : public IErrorStatsSink, public IErrorMetricsQuery {
public:
    /**
     * @brief 初始化数据库管理器
     *
     * 检测数据库类型，创建表（如果不存在），检查并升级表结构
     */
    void init() override;
    
    // ========== 批量写入 ==========
    
    /**
     * @brief 批量插入错误事件到 error_event 表
     * @return 成功返回 true，失败返回 false
     */
    bool insertEvents(const std::vector<ErrorEvent>& events) override;
    
    /**
     * @brief 批量 upsert 错误聚合到 error_agg_hour 表
     * @return 成功返回 true，失败返回 false
     */
    bool upsertErrorAggHour(const std::vector<ErrorEvent>& events) override;
    
    /**
     * @brief upsert 单条请求聚合到 request_agg_hour 表
     * @return 成功返回 true，失败返回 false
     */
    bool upsertRequestAggHour(const RequestAggData& data) override;
    
    // ========== 查询接口 ==========
    
    /**
     * @brief 查询错误时间序列（从 error_agg_hour）
     *
     * @param params 查询参数（包含时间范围和过滤条件）
     * @return 按小时聚合的统计数据
     */
    std::vector<AggBucket> queryErrorSeries(const QueryParams& params) override;
    
    /**
     * @brief 查询请求时间序列（从 request_agg_hour）
     *
     * @param params 查询参数（包含时间范围和过滤条件）
     * @return 按小时聚合的统计数据
     */
    std::vector<AggBucket> queryRequestSeries(const QueryParams& params) override;
    
    /**
     * @brief 查询错误事件明细列表
     *
     * @param params 查询参数（包含时间范围和过滤条件）
     * @param limit 返回数量限制
     * @param offset 偏移量
     * @return 事件列表
     */
    std::vector<ErrorEventRecord> queryEvents(const QueryParams& params, int limit, int offset) override;
    
    /**
     * @brief 根据 ID 查询单条错误事件
     */
    std::optional<ErrorEventRecord> queryEventById(int64_t id) override;
    
    // ========== 清理接口 ==========
    
    /**
     * @brief 删除过期的错误事件明细
     * @return 删除的记录数
     */
    int cleanupOldEvents(int retentionDays) override;
    
    /**
     * @brief 删除过期的聚合数据（error_agg_hour 和 request_agg_hour）
     * @return 删除的记录数
     */
    int cleanupOldAgg(int retentionDays) override;
    
    /**
     * @brief 获取数据库类型
     */
    DbType getDbType() const { return dbType_; }
    
private:
    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    DbType dbType_ = DbType::PostgreSQL;
    
    /**
     * @brief 检测数据库类型
     */
    void detectDbType();
    
    /**
     * @brief 创建表（如果不存在）
     */
    void createTablesIfNotExist();
    
    /**
     * @brief 将时间点截断到整点小时
     */
    std::string truncateBucket(const std::chrono::system_clock::time_point& tp);
};

} // 命名空间结束

#endif // 头文件保护结束
