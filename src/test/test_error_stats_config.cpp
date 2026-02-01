/**
 * @file test_error_stats_config.cpp
 * @brief ErrorStatsConfig 单元测试
 * 
 * 测试内容：
 * - 配置默认值
 * - JSON 加载
 * - DropPolicy 枚举转换
 */

#include <drogon/drogon_test.h>
#include "../metrics/ErrorStatsConfig.h"

using namespace metrics;

// ========== 配置默认值测试 ==========

DROGON_TEST(ErrorStatsConfig_DefaultValues)
{
    ErrorStatsConfig config;
    
    // 功能开关默认值
    CHECK(config.enabled == true);
    CHECK(config.persistDetail == true);
    CHECK(config.persistAgg == true);
    CHECK(config.persistRequestAgg == true);
    
    // 保留策略默认值
    CHECK(config.retentionDaysDetail == 30);
    CHECK(config.retentionDaysAgg == 30);
    CHECK(config.retentionDaysRequestAgg == 30);
    
    // raw_snippet 配置默认值
    CHECK(config.rawSnippetEnabled == true);
    CHECK(config.rawSnippetMaxLen == 32768);
    
    // 异步写入配置默认值
    CHECK(config.asyncBatchSize == 200);
    CHECK(config.asyncFlushMs == 200);
    CHECK(config.dropPolicy == DropPolicy::DROP_OLDEST);
    CHECK(config.queueCapacity == 10000);
}

// ========== DropPolicy 枚举转换测试 ==========

DROGON_TEST(ErrorStatsConfig_DropPolicyToString)
{
    CHECK(ErrorStatsConfig::dropPolicyToString(DropPolicy::DROP_OLDEST) == "drop_oldest");
    CHECK(ErrorStatsConfig::dropPolicyToString(DropPolicy::DROP_NEWEST) == "drop_newest");
}

DROGON_TEST(ErrorStatsConfig_StringToDropPolicy)
{
    CHECK(ErrorStatsConfig::stringToDropPolicy("drop_oldest") == DropPolicy::DROP_OLDEST);
    CHECK(ErrorStatsConfig::stringToDropPolicy("drop_newest") == DropPolicy::DROP_NEWEST);
    CHECK(ErrorStatsConfig::stringToDropPolicy("unknown") == DropPolicy::DROP_OLDEST);  // 默认
    CHECK(ErrorStatsConfig::stringToDropPolicy("") == DropPolicy::DROP_OLDEST);
}

// ========== JSON 加载测试 ==========

DROGON_TEST(ErrorStatsConfig_LoadFromJson_Empty)
{
    Json::Value emptyConfig;
    auto config = ErrorStatsConfig::loadFromJson(emptyConfig);
    
    // 空配置应使用默认值
    CHECK(config.enabled == true);
    CHECK(config.retentionDaysDetail == 30);
    CHECK(config.asyncBatchSize == 200);
}

DROGON_TEST(ErrorStatsConfig_LoadFromJson_Partial)
{
    Json::Value jsonConfig;
    jsonConfig["enabled"] = false;
    jsonConfig["retention_days_detail"] = 7;
    jsonConfig["async_batch_size"] = 100;
    
    auto config = ErrorStatsConfig::loadFromJson(jsonConfig);
    
    // 指定的值应被加载
    CHECK(config.enabled == false);
    CHECK(config.retentionDaysDetail == 7);
    CHECK(config.asyncBatchSize == 100);
    
    // 未指定的值应使用默认值
    CHECK(config.retentionDaysAgg == 30);
    CHECK(config.asyncFlushMs == 200);
}

DROGON_TEST(ErrorStatsConfig_LoadFromJson_Full)
{
    Json::Value jsonConfig;
    jsonConfig["enabled"] = true;
    jsonConfig["persist_detail"] = false;
    jsonConfig["persist_agg"] = false;
    jsonConfig["persist_request_agg"] = false;
    jsonConfig["retention_days_detail"] = 14;
    jsonConfig["retention_days_agg"] = 60;
    jsonConfig["retention_days_request_agg"] = 90;
    jsonConfig["raw_snippet_enabled"] = false;
    jsonConfig["raw_snippet_max_len"] = 16384;
    jsonConfig["async_batch_size"] = 500;
    jsonConfig["async_flush_ms"] = 100;
    jsonConfig["drop_policy"] = "drop_newest";
    jsonConfig["queue_capacity"] = 5000;
    
    auto config = ErrorStatsConfig::loadFromJson(jsonConfig);
    
    CHECK(config.enabled == true);
    CHECK(config.persistDetail == false);
    CHECK(config.persistAgg == false);
    CHECK(config.persistRequestAgg == false);
    CHECK(config.retentionDaysDetail == 14);
    CHECK(config.retentionDaysAgg == 60);
    CHECK(config.retentionDaysRequestAgg == 90);
    CHECK(config.rawSnippetEnabled == false);
    CHECK(config.rawSnippetMaxLen == 16384);
    CHECK(config.asyncBatchSize == 500);
    CHECK(config.asyncFlushMs == 100);
    CHECK(config.dropPolicy == DropPolicy::DROP_NEWEST);
    CHECK(config.queueCapacity == 5000);
}
