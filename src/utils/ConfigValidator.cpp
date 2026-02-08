#include "ConfigValidator.h"
#include <algorithm>

namespace {

bool isPositiveInt(const Json::Value& value) {
    return value.isInt() && value.asInt() > 0;
}

bool isNonNegativeInt(const Json::Value& value) {
    return value.isInt() && value.asInt() >= 0;
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

    if (!config.isMember("db_clients") || !config["db_clients"].isArray() ||
        config["db_clients"].empty()) {
        result.valid = false;
        result.errors.emplace_back("db_clients 必须存在且为非空数组");
    }

    if (!config.isMember("custom_config") || !config["custom_config"].isObject()) {
        result.warnings.emplace_back("custom_config 缺失，部分功能将使用默认配置");
        return result;
    }

    const auto& custom = config["custom_config"];

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
