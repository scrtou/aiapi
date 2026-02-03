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
        // NOTE: Calling getDbClient() without a name tries to fetch the "default" client.
        // Our config defines a named client ("aichatpg") only, so the no-arg overload can
        // trigger an assertion in DbClientManager if no default client exists.
        dbClient_ = drogon::app().getDbClient("aichatpg");
        if (!dbClient_) {
            LOG_ERROR << "[StatusDbManager] Failed to get database client: aichatpg";
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
    LOG_INFO << "[StatusDbManager] DB type: " << (isPostgres_ ? "PostgreSQL" : "SQLite3");
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
    // DEGRADED: 错误率 1% - 10%
    // DOWN: 错误率 >= 10% 或 最近无请求
    if (errorRate < 0.01) {
        return ServiceHealthStatus::OK;
    } else if (errorRate < 0.10) {
        return ServiceHealthStatus::DEGRADED;
    } else {
        return ServiceHealthStatus::DOWN;
    }
}

StatusSummaryData StatusDbManager::getStatusSummary(const StatusQueryParams& params) {
    StatusSummaryData summary;
    
    ensureDbClient();
    if (!dbClient_) {
        LOG_ERROR << "[StatusDbManager] Database client not available";
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
        
        // 查询渠道数量（按 provider 分组）
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
        
        // 查询模型数量（按 model×provider 分组）
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
        
        // 获取渠道状态统计
        auto channels = getChannelStatusList(params);
        for (const auto& ch : channels) {
            switch (ch.status) {
                case ServiceHealthStatus::OK:
                    summary.healthyChannels++;
                    break;
                case ServiceHealthStatus::DEGRADED:
                    summary.degradedChannels++;
                    break;
                case ServiceHealthStatus::DOWN:
                    summary.downChannels++;
                    break;
                default:
                    break;
            }
        }
        
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
        
        // 计算错误率并转换为 vector
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
        LOG_ERROR << "[StatusDbManager] getStatusSummary error: " << e.what();
    }
    
    return summary;
}

std::vector<ChannelStatusData> StatusDbManager::getChannelStatusList(const StatusQueryParams& params) {
    std::vector<ChannelStatusData> channels;
    
    ensureDbClient();
    if (!dbClient_) {
        LOG_ERROR << "[StatusDbManager] Database client not available";
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
            
            // 查询该渠道的时间序列数据
            std::string bucketSql = R"(
                SELECT 
                    bucket_start,
                    COALESCE(SUM(count), 0) as request_count
                FROM request_agg_hour
                WHERE bucket_start >= $1 AND bucket_start < $2
                  AND provider = $3
                GROUP BY bucket_start
                ORDER BY bucket_start
            )";
            
            auto bucketResult = dbClient_->execSqlSync(bucketSql, from, to, channel.channelId);
            
            std::string errBucketSql = R"(
                SELECT 
                    bucket_start,
                    COALESCE(SUM(count), 0) as error_count
                FROM error_agg_hour
                WHERE bucket_start >= $1 AND bucket_start < $2
                  AND provider = $3
                GROUP BY bucket_start
                ORDER BY bucket_start
            )";
            
            auto errBucketResult = dbClient_->execSqlSync(errBucketSql, from, to, channel.channelId);
            
            // 合并数据
            std::map<std::string, StatusBucket> bucketMap;
            for (const auto& brow : bucketResult) {
                std::string bucketStart = brow["bucket_start"].as<std::string>();
                bucketMap[bucketStart].bucketStart = bucketStart;
                bucketMap[bucketStart].requestCount = brow["request_count"].as<int64_t>();
            }
            for (const auto& brow : errBucketResult) {
                std::string bucketStart = brow["bucket_start"].as<std::string>();
                bucketMap[bucketStart].bucketStart = bucketStart;
                bucketMap[bucketStart].errorCount = brow["error_count"].as<int64_t>();
            }
            
            for (auto& [key, bucket] : bucketMap) {
                if (bucket.requestCount > 0) {
                    bucket.errorRate = static_cast<double>(bucket.errorCount) / bucket.requestCount;
                }
                channel.buckets.push_back(bucket);
            }
            
            std::sort(channel.buckets.begin(), channel.buckets.end(),
                [](const StatusBucket& a, const StatusBucket& b) {
                    return a.bucketStart < b.bucketStart;
                });
            
            channels.push_back(channel);
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[StatusDbManager] getChannelStatusList error: " << e.what();
    }
    
    return channels;
}

std::vector<ModelStatusData> StatusDbManager::getModelStatusList(const StatusQueryParams& params) {
    std::vector<ModelStatusData> models;
    
    ensureDbClient();
    if (!dbClient_) {
        LOG_ERROR << "[StatusDbManager] Database client not available";
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
        // 查询每个 model×provider 的请求统计
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
        
        // 查询每个 model×provider 的错误统计
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
        
        // 构建错误数映射 (key: model:provider)
        std::map<std::string, int64_t> errorMap;
        for (const auto& row : errResult) {
            std::string model = row["model"].as<std::string>();
            std::string provider = row["provider"].isNull() ? "" : row["provider"].as<std::string>();
            std::string key = model + ":" + provider;
            errorMap[key] = row["total_errors"].as<int64_t>();
        }
        
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
            
            // 查询该 model×provider 的时间序列数据
            std::string bucketSql = R"(
                SELECT 
                    bucket_start,
                    COALESCE(SUM(count), 0) as request_count
                FROM request_agg_hour
                WHERE bucket_start >= $1 AND bucket_start < $2
                  AND model = $3 AND provider = $4
                GROUP BY bucket_start
                ORDER BY bucket_start
            )";
            
            auto bucketResult = dbClient_->execSqlSync(bucketSql, from, to, modelData.model, modelData.provider);
            
            std::string errBucketSql = R"(
                SELECT 
                    bucket_start,
                    COALESCE(SUM(count), 0) as error_count
                FROM error_agg_hour
                WHERE bucket_start >= $1 AND bucket_start < $2
                  AND model = $3 AND provider = $4
                GROUP BY bucket_start
                ORDER BY bucket_start
            )";
            
            auto errBucketResult = dbClient_->execSqlSync(errBucketSql, from, to, modelData.model, modelData.provider);
            
            // 合并数据
            std::map<std::string, StatusBucket> bucketMap;
            for (const auto& brow : bucketResult) {
                std::string bucketStart = brow["bucket_start"].as<std::string>();
                bucketMap[bucketStart].bucketStart = bucketStart;
                bucketMap[bucketStart].requestCount = brow["request_count"].as<int64_t>();
            }
            for (const auto& brow : errBucketResult) {
                std::string bucketStart = brow["bucket_start"].as<std::string>();
                bucketMap[bucketStart].bucketStart = bucketStart;
                bucketMap[bucketStart].errorCount = brow["error_count"].as<int64_t>();
            }
            
            for (auto& [bkey, bucket] : bucketMap) {
                if (bucket.requestCount > 0) {
                    bucket.errorRate = static_cast<double>(bucket.errorCount) / bucket.requestCount;
                }
                modelData.buckets.push_back(bucket);
            }
            
            std::sort(modelData.buckets.begin(), modelData.buckets.end(),
                [](const StatusBucket& a, const StatusBucket& b) {
                    return a.bucketStart < b.bucketStart;
                });
            
            models.push_back(modelData);
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[StatusDbManager] getModelStatusList error: " << e.what();
    }
    
    return models;
}

} // namespace metrics
