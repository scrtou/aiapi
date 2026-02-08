#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/drogon.h>

/**
 * @brief Bearer Token 认证过滤器，用于保护 /aichat/* 管理接口。
 *
 * 从请求头 Authorization: Bearer <key> 中提取 token，
 * 与 config.json custom_config.admin_api_key 比对。
 * - key 匹配 → 放行
 * - key 不匹配 → 401
 * - admin_api_key 未配置或为空 → 跳过认证（向后兼容），并在首次启动时输出 WARN
 */
class AdminAuthFilter : public drogon::HttpFilter<AdminAuthFilter>
{
public:
    void doFilter(const drogon::HttpRequestPtr &req,
                  drogon::FilterCallback &&fcb,
                  drogon::FilterChainCallback &&fccb) override
    {
        // 从 自定义配置中读取 admin_api_key
        const auto &customConfig = drogon::app().getCustomConfig();
        std::string configuredKey;
        if (customConfig.isMember("admin_api_key") &&
            customConfig["admin_api_key"].isString()) {
            configuredKey = customConfig["admin_api_key"].asString();
        }

        // 如果 admin_api_key 未配置或为空，跳过认证（向后兼容）
        if (configuredKey.empty()) {
            static bool warned = false;
            if (!warned) {
                LOG_WARN << "[AdminAuthFilter] admin_api_key 未配置，管理接口无认证保护！"
                            " 请在 config.json custom_config 中设置 admin_api_key";
                warned = true;
            }
            fccb();
            return;
        }

        // 从请求头中提取 ： <>
        const std::string &authHeader = req->getHeader("Authorization");
        if (authHeader.empty()) {
            auto resp = drogon::HttpResponse::newHttpJsonResponse(
                makeError("Missing Authorization header", "authentication_error"));
            resp->setStatusCode(drogon::k401Unauthorized);
            fcb(resp);
            return;
        }


        const std::string bearerPrefix = "Bearer ";
        if (authHeader.size() <= bearerPrefix.size() ||
            authHeader.compare(0, bearerPrefix.size(), bearerPrefix) != 0) {
            auto resp = drogon::HttpResponse::newHttpJsonResponse(
                makeError("Invalid Authorization format, expected: Bearer <key>",
                          "authentication_error"));
            resp->setStatusCode(drogon::k401Unauthorized);
            fcb(resp);
            return;
        }

        std::string providedKey = authHeader.substr(bearerPrefix.size());

        // 比对 — 使用恒定时间比较防止时序攻击
        if (!constantTimeEqual(providedKey, configuredKey)) {
            auto resp = drogon::HttpResponse::newHttpJsonResponse(
                makeError("Invalid API key", "authentication_error"));
            resp->setStatusCode(drogon::k401Unauthorized);
            fcb(resp);
            return;
        }

        // 认证通过
        fccb();
    }

private:
    static Json::Value makeError(const std::string &message, const std::string &type)
    {
        Json::Value error;
        error["error"]["message"] = message;
        error["error"]["type"] = type;
        return error;
    }

    /// 恒定时间字符串比较，防止时序攻击
    static bool constantTimeEqual(const std::string &a, const std::string &b)
    {
        if (a.size() != b.size()) {
            // 即使长度不同，也遍历一遍以保持时间恒定
            volatile unsigned char result = 1;
            for (size_t i = 0; i < a.size(); ++i) {
                result |= a[i] ^ a[i]; // 无意义操作，仅占时间
            }
            return false;
        }
        volatile unsigned char result = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            result |= static_cast<unsigned char>(a[i]) ^
                      static_cast<unsigned char>(b[i]);
        }
        return result == 0;
    }
};
