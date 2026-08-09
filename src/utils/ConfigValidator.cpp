#include <utils/ConfigValidator.h>
#include <algorithm>
#include <string_view>

namespace {

bool isRetiredProviderKey(std::string_view key)
{
    return key == "nexosapi" || key == "openai";
}

bool isRetiredProviderConfigKey(std::string_view key)
{
    return isRetiredProviderKey(key) || key == "nexos";
}

void validateRetiredProviderReferences(
    const Json::Value& custom,
    ConfigValidator::ValidationResult& result)
{
    const auto reject = [&result](const std::string& path) {
        result.valid = false;
        result.errors.emplace_back(
            path + " 引用了已退役 Provider；请删除该键并迁移到 chaynsapi 或 retoolapi");
    };

    for (const auto& key : custom["providers"].getMemberNames()) {
        if (isRetiredProviderConfigKey(key)) reject("custom_config.providers." + key);
    }
    for (const auto& key : custom["outbound_limits"].getMemberNames()) {
        if (isRetiredProviderKey(key)) reject("custom_config.outbound_limits." + key);
    }
    for (const char* arrayKey : {"login_service_urls", "regist_service_urls",
                                 "downstream_service_api_keys"}) {
        const auto& values = custom[arrayKey];
        if (!values.isArray()) continue;
        for (Json::ArrayIndex i = 0; i < values.size(); ++i) {
            if (values[i]["name"].isString() &&
                isRetiredProviderKey(values[i]["name"].asString())) {
                reject("custom_config." + std::string(arrayKey) + "[" +
                       std::to_string(i) + "].name");
            }
        }
    }
    const auto& accounts = custom["account"];
    if (accounts.isArray()) {
        for (Json::ArrayIndex i = 0; i < accounts.size(); ++i) {
            if (accounts[i]["apiname"].isString() &&
                isRetiredProviderKey(accounts[i]["apiname"].asString())) {
                reject("custom_config.account[" + std::to_string(i) + "].apiname");
            }
        }
    }

    const auto& toolBridge = custom["tool_bridge"];
    if (!toolBridge.isObject()) return;
    for (const char* mapKey : {"format_by_channel", "strict_sentinel_by_channel"}) {
        for (const auto& key : toolBridge[mapKey].getMemberNames()) {
            if (isRetiredProviderKey(key)) {
                reject("custom_config.tool_bridge." + std::string(mapKey) + "." + key);
            }
        }
    }
    for (const char* listKey : {"strict_sentinel_enabled_channels",
                                "strict_sentinel_disabled_channels"}) {
        const auto& values = toolBridge[listKey];
        if (!values.isArray()) continue;
        for (Json::ArrayIndex i = 0; i < values.size(); ++i) {
            if (values[i].isString() && isRetiredProviderKey(values[i].asString())) {
                reject("custom_config.tool_bridge." + std::string(listKey) +
                       "[" + std::to_string(i) + "]");
            }
        }
    }
}

bool isPositiveInt(const Json::Value& value) {
    return value.isInt() && value.asInt() > 0;
}

bool isNonNegativeInt(const Json::Value& value) {
    return value.isInt() && value.asInt() >= 0;
}

/// 小时类配置：允许小数（0.5 = 30 分钟），必须为正数。
bool isPositiveHours(const Json::Value& value) {
    return value.isNumeric() && value.asDouble() > 0.0;
}

bool isValidSessionTrackingMode(const std::string& mode) {
    return mode == "hash" || mode == "zerowidth" || mode == "zero_width";
}

}

ConfigValidator::ValidationResult ConfigValidator::validate(const Json::Value& config) {
    ValidationResult result;

    if (!config.isObject()) {
        result.valid = false;
        result.errors.emplace_back("配置根节点必须是 JSON object");
        return result;
    }

    if (!config.isMember("listeners") || !config["listeners"].isArray() ||
        config["listeners"].empty()) {
        result.valid = false;
        result.errors.emplace_back("listeners 必须存在且为非空数组");
    }

    if (!config.isMember("db_clients") || !config["db_clients"].isArray()) {
        result.warnings.emplace_back("db_clients 缺失或不是数组，将以无内置 DBClient 模式运行");
    }

    if (!config.isMember("custom_config") || !config["custom_config"].isObject()) {
        result.warnings.emplace_back("custom_config 缺失，部分功能将使用默认配置");
        return result;
    }

    const auto& custom = config["custom_config"];
    validateRetiredProviderReferences(custom, result);

    if (custom.isMember("session_tracking") && custom["session_tracking"].isObject()) {
        const auto mode = custom["session_tracking"].get("mode", "hash").asString();
        if (!isValidSessionTrackingMode(mode)) {
            result.valid = false;
            result.errors.emplace_back(
                "custom_config.session_tracking.mode 非法，允许值: hash/zerowidth/zero_width"
            );
        }
    }

    if (custom.isMember("admin_api_key") && custom["admin_api_key"].isString() &&
        custom["admin_api_key"].asString().empty()) {
        result.warnings.emplace_back("admin_api_key 为空，/aichat/* 管理接口将不启用认证");
    }

    if (custom.isMember("response_index") && custom["response_index"].isObject()) {
        const auto& responseIndex = custom["response_index"];
        if (responseIndex.isMember("max_entries") &&
            !isPositiveInt(responseIndex["max_entries"])) {
            result.valid = false;
            result.errors.emplace_back("response_index.max_entries 必须为正整数");
        }
        if (responseIndex.isMember("max_age_hours") &&
            !isPositiveInt(responseIndex["max_age_hours"])) {
            result.valid = false;
            result.errors.emplace_back("response_index.max_age_hours 必须为正整数");
        }
        if (responseIndex.isMember("cleanup_interval_minutes") &&
            !isPositiveInt(responseIndex["cleanup_interval_minutes"])) {
            result.valid = false;
            result.errors.emplace_back("response_index.cleanup_interval_minutes 必须为正整数");
        }
    }

    if (custom.isMember("rate_limit") && custom["rate_limit"].isObject()) {
        const auto& rateLimit = custom["rate_limit"];
        if (rateLimit.get("enabled", false).asBool()) {
            if (!isPositiveInt(rateLimit.get("requests_per_second", Json::Value(0)))) {
                result.valid = false;
                result.errors.emplace_back("rate_limit.requests_per_second 必须为正整数");
            }
            if (!isPositiveInt(rateLimit.get("burst", Json::Value(0)))) {
                result.valid = false;
                result.errors.emplace_back("rate_limit.burst 必须为正整数");
            }
        }
    }

    if (custom.isMember("account_automation") && custom["account_automation"].isObject()) {
        const auto& automation = custom["account_automation"];
        if (automation.isMember("auto_delete_enabled") && !automation["auto_delete_enabled"].isBool()) {
            result.valid = false;
            result.errors.emplace_back("account_automation.auto_delete_enabled 必须为布尔值");
        }
        if (automation.isMember("delete_after_days") &&
            !isPositiveInt(automation["delete_after_days"])) {
            result.valid = false;
            result.errors.emplace_back("account_automation.delete_after_days 必须为正整数");
        }
        if (automation.isMember("auto_register_enabled") && !automation["auto_register_enabled"].isBool()) {
            result.valid = false;
            result.errors.emplace_back("account_automation.auto_register_enabled 必须为布尔值");
        }
    }

    if (custom.isMember("session_persistence") && custom["session_persistence"].isObject()) {
        const auto& sp = custom["session_persistence"];
        const char* hourKeys[] = {"memory_expire_hours",
                                  "memory_cleanup_interval_hours",
                                  "db_retention_hours"};
        for (const char* k : hourKeys) {
            if (!sp.isMember(k)) continue;
            if (!isPositiveHours(sp[k])) {
                result.valid = false;
                result.errors.emplace_back(std::string("session_persistence.") + k + " 必须为正数(小时，可为小数)");
            }
        }
        // 旧版秒级键已废弃：保留告警而非报错，避免老配置直接起不来，但明确提示其不再生效。
        const char* legacyKeys[] = {"memory_expire_seconds",
                                    "memory_cleanup_interval_seconds",
                                    "db_retention_seconds"};
        for (const char* k : legacyKeys) {
            if (sp.isMember(k)) {
                result.warnings.emplace_back(std::string("session_persistence.") + k +
                    " 已废弃且不再生效，请改用对应的 *_hours（单位：小时）");
            }
        }
        const char* boolKeys[] = {"store_session_payload", "store_response_body"};
        for (const char* k : boolKeys) {
            if (sp.isMember(k) && !sp[k].isBool()) {
                result.valid = false;
                result.errors.emplace_back(std::string("session_persistence.") + k + " 必须为布尔值");
            }
        }
        if (isPositiveHours(sp["memory_expire_hours"]) &&
            isPositiveHours(sp["memory_cleanup_interval_hours"]) &&
            sp["memory_cleanup_interval_hours"].asDouble() > sp["memory_expire_hours"].asDouble()) {
            result.valid = false;
            result.errors.emplace_back("session_persistence.memory_cleanup_interval_hours 不得大于 memory_expire_hours");
        }
        if (isPositiveHours(sp["db_retention_hours"]) && isPositiveHours(sp["memory_expire_hours"]) &&
            sp["db_retention_hours"].asDouble() < sp["memory_expire_hours"].asDouble()) {
            result.warnings.emplace_back("session_persistence.db_retention_hours 小于 memory_expire_hours，"
                "活跃会话的 DB 快照会被提前清理，重启后无法懒加载恢复");
        }
    }

    if (custom.isMember("tool_bridge") && custom["tool_bridge"].isObject()) {
        const auto& toolBridge = custom["tool_bridge"];
        if (toolBridge.isMember("namespace_enabled") && !toolBridge["namespace_enabled"].isBool()) {
            result.valid = false;
            result.errors.emplace_back("tool_bridge.namespace_enabled 必须为布尔值");
        }
    }

    if (custom.isMember("error_stats") && custom["error_stats"].isObject()) {
        const auto& stats = custom["error_stats"];
        if (stats.isMember("retention_days_detail") &&
            !isNonNegativeInt(stats["retention_days_detail"])) {
            result.valid = false;
            result.errors.emplace_back("error_stats.retention_days_detail 必须为非负整数");
        }
    }

    return result;
}
