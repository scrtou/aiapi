#include "StatusDbManager.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace metrics {

std::shared_ptr<StatusDbManager> StatusDbManager::getInstance() {
    static std::shared_ptr<StatusDbManager> instance = std::make_shared<StatusDbManager>();
    return instance;
}

void StatusDbManager::init() {
    // 延迟初始化：只在首次需要时获取数据库客户端
}

void StatusDbManager::ensureDbClient() {
    if (!dbClient_) {
        detectDbType();
        // 从 Drogon 默认连接池获取数据库客户端 aichatpg。
        dbClient_ = drogon::app().getDbClient("aichatpg");
        if (!dbClient_) {
            LOG_ERROR << "[状态数据库管理器] 获取数据库客户端失败";
        }
    }
}

void StatusDbManager::detectDbType() {
    auto customConfig = drogon::app().getCustomConfig();
    std::string dbTypeStr = "postgresql";
    if (customConfig.isMember("dbtype")) {
        dbTypeStr = customConfig["dbtype"].asString();
    }
    std::transform(dbTypeStr.begin(), dbTypeStr.end(), dbTypeStr.begin(), ::tolower);
    isPostgres_ = !(dbTypeStr == "sqlite3" || dbTypeStr == "sqlite");
    LOG_INFO << "[状态数据库管理器] 数据库类型：" << (isPostgres_ ? "PostgreSQL/MySQL" : "SQLite3");
}

std::string StatusDbManager::formatTimeUtc(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::gmtime(&tt));
    return std::string(buf);
}

std::pair<std::string, std::string> StatusDbManager::getDefaultTimeRange() {
    auto now = std::chrono::system_clock::now();
    auto yesterday = now - std::chrono::hours(24);
    return {formatTimeUtc(yesterday), formatTimeUtc(now)};
}

std::string StatusDbManager::statusToString(ServiceHealthStatus status) {
    switch (status) {
        case ServiceHealthStatus::OK: return "OK";
        case ServiceHealthStatus::DEGRADED: return "DEGRADED";
        case ServiceHealthStatus::DOWN: return "DOWN";
        default: return "UNKNOWN";
    }
}

ServiceHealthStatus StatusDbManager::calculateStatus(double errorRate, int64_t requestCount) {
    // 如果请求数太少，无法判断状态
    if (requestCount < 5) {
        return ServiceHealthStatus::UNKNOWN;
    }
    
    // 状态判定阈值（可配置）
    // OK: 错误率 < 1%

    // DOWN： 错误率 >= 10% 或 最近无请求
    if (errorRate < 0.01) {
        return ServiceHealthStatus::OK;
    } else if (errorRate < 0.10) {
        return ServiceHealthStatus::DEGRADED;
    } else {
        return ServiceHealthStatus::DOWN;
    }
}

// ============================================================================
// 优化：一次性查询所有渠道的时间序列数据，避免 N+1 查询问题
// ============================================================================
std::map<std::string, std::vector<StatusBucket>> StatusDbManager::fetchAllChannelBuckets(
    const std::string& from, const std::string& to) {
    
    std::map<std::string, std::vector<StatusBucket>> result;
    
    try {
        // 一次性查询所有上游的请求时间序列
        std::string reqBucketSql = R"(
            SELECT 
                provider,
                bucket_start,
                COALESCE(SUM(count), 0) as request_count
            FROM request_agg_hour
            WHERE bucket_start >= $1 AND bucket_start < $2
              AND provider IS NOT NULL AND provider != ''
            GROUP BY provider, bucket_start
            ORDER BY provider, bucket_start
        )";
        
        auto reqResult = dbClient_->execSqlSync(reqBucketSql, from, to);
        
        // 一次性查询所有上游的错误时间序列
        std::string errBucketSql = R"(
            SELECT 
                provider,
                bucket_start,
                COALESCE(SUM(count), 0) as error_count
            FROM error_agg_hour
            WHERE bucket_start >= $1 AND bucket_start < $2
              AND provider IS NOT NULL AND provider != ''
            GROUP BY provider, bucket_start
            ORDER BY provider, bucket_start
        )";
        
        auto errResult = dbClient_->execSqlSync(errBucketSql, from, to);
        
        // 在内存中组织数据：上游 -> bucket_start -> StatusBucket
        std::map<std::string, std::map<std::string, StatusBucket>> providerBucketMap;
        
        for (const auto& row : reqResult) {
            std::string provider = row["provider"].as<std::string>();
            std::string bucketStart = row["bucket_start"].as<std::string>();
            providerBucketMap[provider][bucketStart].bucketStart = bucketStart;
            providerBucketMap[provider][bucketStart].requestCount = row["request_count"].as<int64_t>();
        }
        
        for (const auto& row : errResult) {
            std::string provider = row["provider"].as<std::string>();
            std::string bucketStart = row["bucket_start"].as<std::string>();
            providerBucketMap[provider][bucketStart].bucketStart = bucketStart;
            providerBucketMap[provider][bucketStart].errorCount = row["error_count"].as<int64_t>();
        }
        
        // 转换为最终结果格式，计算错误率
        for (auto& [provider, bucketMap] : providerBucketMap) {
            std::vector<StatusBucket> buckets;
            for (auto& [bucketStart, bucket] : bucketMap) {
                if (bucket.requestCount > 0) {
                    bucket.errorRate = static_cast<double>(bucket.errorCount) / bucket.requestCount;
                }
                buckets.push_back(bucket);
            }
            // 按时间排序
            std::sort(buckets.begin(), buckets.end(),
                [](const StatusBucket& a, const StatusBucket& b) {
                    return a.bucketStart < b.bucketStart;
                });
            result[provider] = std::move(buckets);
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[状态数据库管理器] 查询全部渠道时间序列失败：" << e.what();
    }
    
    return result;
}

// ============================================================================
// 优化：一次性查询所有模型的时间序列数据，避免 N+1 查询问题
// ============================================================================
std::map<std::string, std::vector<StatusBucket>> StatusDbManager::fetchAllModelBuckets(
    const std::string& from, const std::string& to) {
    
    std::map<std::string, std::vector<StatusBucket>> result;
    
    try {
        // 构建 模型：上游 组合键的 SQL（兼容 PG 和 SQLite）
        std::string reqBucketSql;
        std::string errBucketSql;
        
        if (isPostgres_) {
            reqBucketSql = R"(
                SELECT 
                    model,
                    provider,
                    bucket_start,
                    COALESCE(SUM(count), 0) as request_count
                FROM request_agg_hour
                WHERE bucket_start >= $1 AND bucket_start < $2
                  AND model IS NOT NULL AND model != ''
                GROUP BY model, provider, bucket_start
                ORDER BY model, provider, bucket_start
            )";
            
            errBucketSql = R"(
                SELECT 
                    model,
                    provider,
                    bucket_start,
                    COALESCE(SUM(count), 0) as error_count
                FROM error_agg_hour
                WHERE bucket_start >= $1 AND bucket_start < $2
                  AND model IS NOT NULL AND model != ''
                GROUP BY model, provider, bucket_start
                ORDER BY model, provider, bucket_start
            )";
        } else {

            reqBucketSql = R"(
                SELECT 
                    model,
                    provider,
                    bucket_start,
                    COALESCE(SUM(count), 0) as request_count
                FROM request_agg_hour
                WHERE bucket_start >= $1 AND bucket_start < $2
                  AND model IS NOT NULL AND model != ''
                GROUP BY model, provider, bucket_start
                ORDER BY model, provider, bucket_start
            )";
            
            errBucketSql = R"(
                SELECT 
                    model,
                    provider,
                    bucket_start,
                    COALESCE(SUM(count), 0) as error_count
                FROM error_agg_hour
                WHERE bucket_start >= $1 AND bucket_start < $2
                  AND model IS NOT NULL AND model != ''
                GROUP BY model, provider, bucket_start
                ORDER BY model, provider, bucket_start
            )";
        }
        
        auto reqResult = dbClient_->execSqlSync(reqBucketSql, from, to);
        auto errResult = dbClient_->execSqlSync(errBucketSql, from, to);
        
        // 在内存中组织数据：(模型：上游) -> bucket_start -> StatusBucket
        std::map<std::string, std::map<std::string, StatusBucket>> modelBucketMap;
        
        for (const auto& row : reqResult) {
            std::string model = row["model"].as<std::string>();
            std::string provider = row["provider"].isNull() ? "" : row["provider"].as<std::string>();
            std::string key = model + ":" + provider;
            std::string bucketStart = row["bucket_start"].as<std::string>();
            modelBucketMap[key][bucketStart].bucketStart = bucketStart;
            modelBucketMap[key][bucketStart].requestCount = row["request_count"].as<int64_t>();
        }
        
        for (const auto& row : errResult) {
            std::string model = row["model"].as<std::string>();
            std::string provider = row["provider"].isNull() ? "" : row["provider"].as<std::string>();
            std::string key = model + ":" + provider;
            std::string bucketStart = row["bucket_start"].as<std::string>();
            modelBucketMap[key][bucketStart].bucketStart = bucketStart;
            modelBucketMap[key][bucketStart].errorCount = row["error_count"].as<int64_t>();
        }
        
        // 转换为最终结果格式，计算错误率
        for (auto& [modelKey, bucketMap] : modelBucketMap) {
            std::vector<StatusBucket> buckets;
            for (auto& [bucketStart, bucket] : bucketMap) {
                if (bucket.requestCount > 0) {
                    bucket.errorRate = static_cast<double>(bucket.errorCount) / bucket.requestCount;
                }
                buckets.push_back(bucket);
            }
            // 按时间排序
            std::sort(buckets.begin(), buckets.end(),
                [](const StatusBucket& a, const StatusBucket& b) {
                    return a.bucketStart < b.bucketStart;
                });
            result[modelKey] = std::move(buckets);
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[状态数据库管理器] 查询全部模型时间序列失败：" << e.what();
    }
    
    return result;
}

// ============================================================================
// 优化：轻量级渠道状态统计，避免 getStatusSummary 中的重复查询
// ============================================================================
ChannelStatusCounts StatusDbManager::getChannelStatusCounts(const std::string& from, const std::string& to) {
    ChannelStatusCounts counts;
    
    try {
        // 一次查询获取每个渠道的请求数和错误数，然后计算状态
        std::string sql = R"(
            SELECT 
                r.provider,
                COALESCE(SUM(r.count), 0) as total_requests,
                COALESCE(e.total_errors, 0) as total_errors
            FROM request_agg_hour r
            LEFT JOIN (
                SELECT provider, SUM(count) as total_errors
                FROM error_agg_hour
                WHERE bucket_start >= $1 AND bucket_start < $2
                  AND provider IS NOT NULL AND provider != ''
                GROUP BY provider
            ) e ON r.provider = e.provider
            WHERE r.bucket_start >= $1 AND r.bucket_start < $2
              AND r.provider IS NOT NULL AND r.provider != ''
            GROUP BY r.provider
        )";
        
        auto result = dbClient_->execSqlSync(sql, from, to);
        
        for (const auto& row : result) {
            int64_t requests = row["total_requests"].as<int64_t>();
            int64_t errors = row["total_errors"].isNull() ? 0 : row["total_errors"].as<int64_t>();
            
            double errorRate = (requests > 0) ? static_cast<double>(errors) / requests : 0.0;
            ServiceHealthStatus status = calculateStatus(errorRate, requests);
            
            switch (status) {
                case ServiceHealthStatus::OK:
                    counts.healthy++;
                    break;
                case ServiceHealthStatus::DEGRADED:
                    counts.degraded++;
                    break;
                case ServiceHealthStatus::DOWN:
                    counts.down++;
                    break;
                default:
                    break;
            }
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[状态数据库管理器] 获取渠道状态计数失败：" << e.what();
    }
    
    return counts;
}

StatusSummaryData StatusDbManager::getStatusSummary(const StatusQueryParams& params) {
    StatusSummaryData summary;
    
    ensureDbClient();
    if (!dbClient_) {
        LOG_ERROR << "[状态数据库管理器] 数据库客户端不可用";
        return summary;
    }
    
    std::string from = params.from;
    std::string to = params.to;
    if (from.empty() || to.empty()) {
        auto [defaultFrom, defaultTo] = getDefaultTimeRange();
        if (from.empty()) from = defaultFrom;
        if (to.empty()) to = defaultTo;
    }
    
    try {
        // 查询总请求数
        std::string reqSql = R"(
            SELECT COALESCE(SUM(count), 0) as total_requests
            FROM request_agg_hour
            WHERE bucket_start >= $1 AND bucket_start < $2
        )";
        
        auto reqResult = dbClient_->execSqlSync(reqSql, from, to);
        if (!reqResult.empty()) {
            summary.totalRequests = reqResult[0]["total_requests"].as<int64_t>();
        }
        
        // 查询总错误数
        std::string errSql = R"(
            SELECT COALESCE(SUM(count), 0) as total_errors
            FROM error_agg_hour
            WHERE bucket_start >= $1 AND bucket_start < $2
        )";
        
        auto errResult = dbClient_->execSqlSync(errSql, from, to);
        if (!errResult.empty()) {
            summary.totalErrors = errResult[0]["total_errors"].as<int64_t>();
        }
        
        // 计算错误率
        if (summary.totalRequests > 0) {
            summary.errorRate = static_cast<double>(summary.totalErrors) / summary.totalRequests;
        }
        
        // 查询渠道数量（按 上游 分组）
        std::string channelSql = R"(
            SELECT COUNT(DISTINCT provider) as channel_count
            FROM request_agg_hour
            WHERE bucket_start >= $1 AND bucket_start < $2
              AND provider IS NOT NULL AND provider != ''
        )";
        
        auto channelResult = dbClient_->execSqlSync(channelSql, from, to);
        if (!channelResult.empty()) {
            summary.channelCount = channelResult[0]["channel_count"].as<int>();
        }
        
        // 查询模型数量（按 模型×上游 分组）
        std::string modelSql = R"(
            SELECT COUNT(DISTINCT CONCAT(model, ':', provider)) as model_count
            FROM request_agg_hour
            WHERE bucket_start >= $1 AND bucket_start < $2
              AND model IS NOT NULL AND model != ''
        )";
        
        // SQLite 不支持 CONCAT，使用 || 替代
        if (!isPostgres_) {
            modelSql = R"(
                SELECT COUNT(DISTINCT (model || ':' || provider)) as model_count
                FROM request_agg_hour
                WHERE bucket_start >= $1 AND bucket_start < $2
                  AND model IS NOT NULL AND model != ''
            )";
        }
        
        auto modelResult = dbClient_->execSqlSync(modelSql, from, to);
        if (!modelResult.empty()) {
            summary.modelCount = modelResult[0]["model_count"].as<int>();
        }
        
        // 优化：使用轻量级方法获取渠道状态统计，避免 N+1 查询
        auto channelCounts = getChannelStatusCounts(from, to);
        summary.healthyChannels = channelCounts.healthy;
        summary.degradedChannels = channelCounts.degraded;
        summary.downChannels = channelCounts.down;
        
        // 计算整体状态
        summary.overallStatus = calculateStatus(summary.errorRate, summary.totalRequests);
        
        // 查询时间序列数据
        std::string bucketSql = R"(
            SELECT 
                bucket_start,
                COALESCE(SUM(count), 0) as request_count
            FROM request_agg_hour
            WHERE bucket_start >= $1 AND bucket_start < $2
            GROUP BY bucket_start
            ORDER BY bucket_start
        )";
        
        auto bucketResult = dbClient_->execSqlSync(bucketSql, from, to);
        
        // 同时查询错误时间序列
        std::string errBucketSql = R"(
            SELECT 
                bucket_start,
                COALESCE(SUM(count), 0) as error_count
            FROM error_agg_hour
            WHERE bucket_start >= $1 AND bucket_start < $2
            GROUP BY bucket_start
            ORDER BY bucket_start
        )";
        
        auto errBucketResult = dbClient_->execSqlSync(errBucketSql, from, to);
        
        // 合并请求和错误数据
        std::map<std::string, StatusBucket> bucketMap;
        for (const auto& row : bucketResult) {
            std::string bucketStart = row["bucket_start"].as<std::string>();
            bucketMap[bucketStart].bucketStart = bucketStart;
            bucketMap[bucketStart].requestCount = row["request_count"].as<int64_t>();
        }
        for (const auto& row : errBucketResult) {
            std::string bucketStart = row["bucket_start"].as<std::string>();
            bucketMap[bucketStart].bucketStart = bucketStart;
            bucketMap[bucketStart].errorCount = row["error_count"].as<int64_t>();
        }
        
        // 计算错误率并转换为
        for (auto& [key, bucket] : bucketMap) {
            if (bucket.requestCount > 0) {
                bucket.errorRate = static_cast<double>(bucket.errorCount) / bucket.requestCount;
            }
            summary.buckets.push_back(bucket);
        }
        
        // 按时间排序
        std::sort(summary.buckets.begin(), summary.buckets.end(),
            [](const StatusBucket& a, const StatusBucket& b) {
                return a.bucketStart < b.bucketStart;
            });
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[状态数据库管理器] 获取状态总览失败：" << e.what();
    }
    
    return summary;
}

std::vector<ChannelStatusData> StatusDbManager::getChannelStatusList(const StatusQueryParams& params) {
    std::vector<ChannelStatusData> channels;
    
    ensureDbClient();
    if (!dbClient_) {
        LOG_ERROR << "[状态数据库管理器] 数据库客户端不可用";
        return channels;
    }
    
    std::string from = params.from;
    std::string to = params.to;
    if (from.empty() || to.empty()) {
        auto [defaultFrom, defaultTo] = getDefaultTimeRange();
        if (from.empty()) from = defaultFrom;
        if (to.empty()) to = defaultTo;
    }
    
    try {
        // 查询每个渠道的请求统计
        std::string sql = R"(
            SELECT 
                provider,
                COALESCE(SUM(count), 0) as total_requests,
                MAX(last_request_ts) as last_request_time
            FROM request_agg_hour
            WHERE bucket_start >= $1 AND bucket_start < $2
              AND provider IS NOT NULL AND provider != ''
            GROUP BY provider
            ORDER BY total_requests DESC
        )";
        
        auto result = dbClient_->execSqlSync(sql, from, to);
        
        // 查询每个渠道的错误统计
        std::string errSql = R"(
            SELECT 
                provider,
                COALESCE(SUM(count), 0) as total_errors
            FROM error_agg_hour
            WHERE bucket_start >= $1 AND bucket_start < $2
              AND provider IS NOT NULL AND provider != ''
            GROUP BY provider
        )";
        
        auto errResult = dbClient_->execSqlSync(errSql, from, to);
        
        // 构建错误数映射
        std::map<std::string, int64_t> errorMap;
        for (const auto& row : errResult) {
            std::string provider = row["provider"].as<std::string>();
            errorMap[provider] = row["total_errors"].as<int64_t>();
        }
        
        // 优化：一次性获取所有渠道的时间序列数据，避免 N+1 查询
        auto allChannelBuckets = fetchAllChannelBuckets(from, to);
        
        // 构建渠道状态列表
        for (const auto& row : result) {
            ChannelStatusData channel;
            channel.channelId = row["provider"].as<std::string>();
            channel.channelName = channel.channelId; // 目前没有渠道名称，使用 ID
            channel.totalRequests = row["total_requests"].as<int64_t>();
            
            if (!row["last_request_time"].isNull()) {
                channel.lastRequestTime = row["last_request_time"].as<std::string>();
            }
            
            // 获取错误数
            auto it = errorMap.find(channel.channelId);
            if (it != errorMap.end()) {
                channel.totalErrors = it->second;
            }
            
            // 计算错误率和状态
            if (channel.totalRequests > 0) {
                channel.errorRate = static_cast<double>(channel.totalErrors) / channel.totalRequests;
            }
            channel.status = calculateStatus(channel.errorRate, channel.totalRequests);
            
            // 从预取的数据中获取该渠道的时间序列
            auto bucketsIt = allChannelBuckets.find(channel.channelId);
            if (bucketsIt != allChannelBuckets.end()) {
                channel.buckets = bucketsIt->second;
            }
            
            channels.push_back(channel);
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[状态数据库管理器] 获取渠道状态列表失败：" << e.what();
    }
    
    return channels;
}

std::vector<ModelStatusData> StatusDbManager::getModelStatusList(const StatusQueryParams& params) {
    std::vector<ModelStatusData> models;
    
    ensureDbClient();
    if (!dbClient_) {
        LOG_ERROR << "[状态数据库管理器] 数据库客户端不可用";
        return models;
    }
    
    std::string from = params.from;
    std::string to = params.to;
    if (from.empty() || to.empty()) {
        auto [defaultFrom, defaultTo] = getDefaultTimeRange();
        if (from.empty()) from = defaultFrom;
        if (to.empty()) to = defaultTo;
    }
    
    try {
        // 查询每个 模型×上游 的请求统计
        std::string sql = R"(
            SELECT 
                model,
                provider,
                COALESCE(SUM(count), 0) as total_requests,
                MAX(last_request_ts) as last_request_time
            FROM request_agg_hour
            WHERE bucket_start >= $1 AND bucket_start < $2
              AND model IS NOT NULL AND model != ''
            GROUP BY model, provider
            ORDER BY total_requests DESC
        )";
        
        auto result = dbClient_->execSqlSync(sql, from, to);
        
        // 查询每个 模型×上游 的错误统计
        std::string errSql = R"(
            SELECT 
                model,
                provider,
                COALESCE(SUM(count), 0) as total_errors
            FROM error_agg_hour
            WHERE bucket_start >= $1 AND bucket_start < $2
              AND model IS NOT NULL AND model != ''
            GROUP BY model, provider
        )";
        
        auto errResult = dbClient_->execSqlSync(errSql, from, to);
        
        // 构建错误数映射（键：模型:上游）
        std::map<std::string, int64_t> errorMap;
        for (const auto& row : errResult) {
            std::string model = row["model"].as<std::string>();
            std::string provider = row["provider"].isNull() ? "" : row["provider"].as<std::string>();
            std::string key = model + ":" + provider;
            errorMap[key] = row["total_errors"].as<int64_t>();
        }
        
        // 优化：一次性获取所有模型的时间序列数据，避免 N+1 查询
        auto allModelBuckets = fetchAllModelBuckets(from, to);
        
        // 构建模型状态列表
        for (const auto& row : result) {
            ModelStatusData modelData;
            modelData.model = row["model"].as<std::string>();
            modelData.provider = row["provider"].isNull() ? "" : row["provider"].as<std::string>();
            modelData.totalRequests = row["total_requests"].as<int64_t>();
            
            if (!row["last_request_time"].isNull()) {
                modelData.lastRequestTime = row["last_request_time"].as<std::string>();
            }
            
            // 获取错误数
            std::string key = modelData.model + ":" + modelData.provider;
            auto it = errorMap.find(key);
            if (it != errorMap.end()) {
                modelData.totalErrors = it->second;
            }
            
            // 计算错误率和状态
            if (modelData.totalRequests > 0) {
                modelData.errorRate = static_cast<double>(modelData.totalErrors) / modelData.totalRequests;
            }
            modelData.status = calculateStatus(modelData.errorRate, modelData.totalRequests);
            
            // 从预取的数据中获取该模型的时间序列
            auto bucketsIt = allModelBuckets.find(key);
            if (bucketsIt != allModelBuckets.end()) {
                modelData.buckets = bucketsIt->second;
            }
            
            models.push_back(modelData);
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[状态数据库管理器] 获取模型状态列表失败：" << e.what();
    }
    
    return models;
}

} // 命名空间结束
