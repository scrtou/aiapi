#include "ErrorStatsConfig.h"
#include <drogon/drogon.h>
#include <mutex>

namespace metrics {

// 全局配置实例
static ErrorStatsConfig g_config;
static std::once_flag g_initFlag;

ErrorStatsConfig ErrorStatsConfig::loadFromJson(const Json::Value& config) {
    ErrorStatsConfig cfg;
    
    if (config.isNull() || !config.isObject()) {
        LOG_WARN << "[错误统计配置] error_stats 配置缺失或无效，使用默认配置";
        return cfg;
    }
    
    // 功能开关
    if (config.isMember("enabled") && config["enabled"].isBool()) {
        cfg.enabled = config["enabled"].asBool();
    }
    if (config.isMember("persist_detail") && config["persist_detail"].isBool()) {
        cfg.persistDetail = config["persist_detail"].asBool();
    }
    if (config.isMember("persist_agg") && config["persist_agg"].isBool()) {
        cfg.persistAgg = config["persist_agg"].asBool();
    }
    if (config.isMember("persist_request_agg") && config["persist_request_agg"].isBool()) {
        cfg.persistRequestAgg = config["persist_request_agg"].asBool();
    }
    
    // 保留策略
    if (config.isMember("retention_days_detail") && config["retention_days_detail"].isInt()) {
        cfg.retentionDaysDetail = config["retention_days_detail"].asInt();
        if (cfg.retentionDaysDetail < 1) cfg.retentionDaysDetail = 1;
        if (cfg.retentionDaysDetail > 365) cfg.retentionDaysDetail = 365;
    }
    if (config.isMember("retention_days_agg") && config["retention_days_agg"].isInt()) {
        cfg.retentionDaysAgg = config["retention_days_agg"].asInt();
        if (cfg.retentionDaysAgg < 1) cfg.retentionDaysAgg = 1;
        if (cfg.retentionDaysAgg > 365) cfg.retentionDaysAgg = 365;
    }
    if (config.isMember("retention_days_request_agg") && config["retention_days_request_agg"].isInt()) {
        cfg.retentionDaysRequestAgg = config["retention_days_request_agg"].asInt();
        if (cfg.retentionDaysRequestAgg < 1) cfg.retentionDaysRequestAgg = 1;
        if (cfg.retentionDaysRequestAgg > 365) cfg.retentionDaysRequestAgg = 365;
    }
    

    if (config.isMember("raw_snippet_enabled") && config["raw_snippet_enabled"].isBool()) {
        cfg.rawSnippetEnabled = config["raw_snippet_enabled"].asBool();
    }
    if (config.isMember("raw_snippet_max_len") && config["raw_snippet_max_len"].isUInt64()) {
        cfg.rawSnippetMaxLen = static_cast<size_t>(config["raw_snippet_max_len"].asUInt64());
        // 限制范围：1KB ~ 1MB
        if (cfg.rawSnippetMaxLen < 1024) cfg.rawSnippetMaxLen = 1024;
        if (cfg.rawSnippetMaxLen > 1048576) cfg.rawSnippetMaxLen = 1048576;
    }
    
    // 异步写入配置
    if (config.isMember("async_batch_size") && config["async_batch_size"].isUInt64()) {
        cfg.asyncBatchSize = static_cast<size_t>(config["async_batch_size"].asUInt64());
        if (cfg.asyncBatchSize < 1) cfg.asyncBatchSize = 1;
        if (cfg.asyncBatchSize > 10000) cfg.asyncBatchSize = 10000;
    }
    if (config.isMember("async_flush_ms") && config["async_flush_ms"].isUInt64()) {
        cfg.asyncFlushMs = static_cast<size_t>(config["async_flush_ms"].asUInt64());
        if (cfg.asyncFlushMs < 10) cfg.asyncFlushMs = 10;
        if (cfg.asyncFlushMs > 60000) cfg.asyncFlushMs = 60000;
    }
    if (config.isMember("drop_policy") && config["drop_policy"].isString()) {
        cfg.dropPolicy = stringToDropPolicy(config["drop_policy"].asString());
    }
    if (config.isMember("queue_capacity") && config["queue_capacity"].isUInt64()) {
        cfg.queueCapacity = static_cast<size_t>(config["queue_capacity"].asUInt64());
        if (cfg.queueCapacity < 100) cfg.queueCapacity = 100;
        if (cfg.queueCapacity > 1000000) cfg.queueCapacity = 1000000;
    }
    
    LOG_INFO << "[错误统计配置] 已加载配置： =" << cfg.enabled
             << ", persistDetail=" << cfg.persistDetail
             << ", persistAgg=" << cfg.persistAgg
             << ", persistRequestAgg=" << cfg.persistRequestAgg
             << ", retentionDaysDetail=" << cfg.retentionDaysDetail
             << ", rawSnippetEnabled=" << cfg.rawSnippetEnabled
             << ", rawSnippetMaxLen=" << cfg.rawSnippetMaxLen
             << ", asyncBatchSize=" << cfg.asyncBatchSize
             << ", asyncFlushMs=" << cfg.asyncFlushMs
             << ", queueCapacity=" << cfg.queueCapacity;
    
    return cfg;
}

ErrorStatsConfig& ErrorStatsConfig::getInstance() {
    return g_config;
}

void ErrorStatsConfig::initFromApp() {
    std::call_once(g_initFlag, []() {
        const auto& customConfig = drogon::app().getCustomConfig();
        if (customConfig.isMember("error_stats")) {
            g_config = loadFromJson(customConfig["error_stats"]);
        } else {
            LOG_WARN << "[错误统计配置] custom_config.error_stats 未找到，使用默认配置";
        }
    });
}

} // 命名空间结束
