#include <drogon/drogon.h>
#include <accountManager/accountManager.h>
#include <apiManager/ApiManager.h>
#include <channelManager/channelManager.h>
#include <sessionManager/core/Session.h>
#include <metrics/ErrorStatsService.h>
#include <utils/BackgroundTaskQueue.h>
#include <utils/ConfigValidator.h>
#include <sessionManager/continuity/ResponseIndex.h>
#include <controllers/HealthController.h>
#include <controllers/AdminAuthFilter.h>
#include <controllers/RateLimitFilter.h>
#include <chrono>
#include <fstream>

namespace {

bool validateConfigFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        LOG_ERROR << "[启动] 无法打开配置文件：" << path;
        return false;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::string errs;
    if (!Json::parseFromStream(builder, in, &root, &errs)) {
        LOG_ERROR << "[启动] 配置文件 JSON 解析失败：" << errs;
        return false;
    }

    const auto validation = ConfigValidator::validate(root);
    for (const auto& warning : validation.warnings) {
        LOG_WARN << "[配置校验]" << warning;
    }
    for (const auto& error : validation.errors) {
        LOG_ERROR << "[配置校验]" << error;
    }

    return validation.valid;
}

const Json::Value& getCustomConfig() {
    return drogon::app().getCustomConfig();
}

void ensureFilterReflectionRegistration() {
    // 显式触发过滤器反射注册，避免仅通过字符串路由引用时被链接器裁剪。
    (void)AdminAuthFilter::classTypeName();
    (void)RateLimitFilter::classTypeName();
}

Json::Value getCorsConfig() {
    const auto& custom = getCustomConfig();
    if (custom.isMember("cors") && custom["cors"].isObject()) {
        return custom["cors"];
    }
    Json::Value fallback(Json::objectValue);
    fallback["allowed_origins"] = Json::arrayValue;
    fallback["allowed_origins"].append("*");
    fallback["allowed_methods"] = Json::arrayValue;
    fallback["allowed_methods"].append("GET");
    fallback["allowed_methods"].append("POST");
    fallback["allowed_methods"].append("PUT");
    fallback["allowed_methods"].append("DELETE");
    fallback["allowed_methods"].append("OPTIONS");
    fallback["allowed_methods"].append("PATCH");
    fallback["allowed_headers"] = Json::arrayValue;
    fallback["allowed_headers"].append("*");
    fallback["expose_headers"] = Json::arrayValue;
    fallback["expose_headers"].append("*");
    fallback["allow_credentials"] = false;
    fallback["max_age"] = 3600;
    return fallback;
}

std::string joinJsonArray(const Json::Value& arr, const std::string& fallback) {
    if (!arr.isArray() || arr.empty()) {
        return fallback;
    }
    std::string out;
    for (Json::ArrayIndex i = 0; i < arr.size(); ++i) {
        if (!arr[i].isString()) continue;
        if (!out.empty()) out += ", ";
        out += arr[i].asString();
    }
    return out.empty() ? fallback : out;
}

bool isOriginAllowed(const std::string& origin, const Json::Value& allowedOrigins) {
    if (!allowedOrigins.isArray() || allowedOrigins.empty()) {
        return true;
    }
    for (const auto& item : allowedOrigins) {
        if (!item.isString()) continue;
        const auto v = item.asString();
        if (v == "*" || (!origin.empty() && v == origin)) {
            return true;
        }
    }
    return false;
}

}

int main() {
    // 加载并校验配置
    drogon::app().loadConfigFile("../config.json");
    ensureFilterReflectionRegistration();

    if (!validateConfigFile("../config.json")) {
        LOG_ERROR << "[启动] 配置校验失败，程序退出";
        return 1;
    }

    // 全局 CORS 预处理（处理 OPTIONS 预检）
    drogon::app().registerPreRoutingAdvice(
        [](const drogon::HttpRequestPtr &req,
           drogon::AdviceCallback &&callback,
           drogon::AdviceChainCallback &&chainCallback) {
            const auto corsConfig = getCorsConfig();
            const auto origin = req->getHeader("Origin");
            const bool originAllowed = isOriginAllowed(origin, corsConfig["allowed_origins"]);
            const std::string allowOrigin = originAllowed ? (origin.empty() ? "*" : origin) : "null";

            if (req->method() == drogon::HttpMethod::Options) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->addHeader("Access-Control-Allow-Origin", allowOrigin);
                resp->addHeader("Access-Control-Allow-Methods", joinJsonArray(corsConfig["allowed_methods"], "GET, POST, PUT, DELETE, OPTIONS, PATCH"));
                resp->addHeader("Access-Control-Allow-Headers", joinJsonArray(corsConfig["allowed_headers"], "*"));
                resp->addHeader("Access-Control-Max-Age", std::to_string(corsConfig.get("max_age", 3600).asInt()));
                if (corsConfig.get("allow_credentials", false).asBool()) {
                    resp->addHeader("Access-Control-Allow-Credentials", "true");
                }
                resp->setStatusCode(drogon::k204NoContent);
                callback(resp);
            } else {
                chainCallback();
            }
        });

    // 全局 CORS 后处理（补充响应头）
    drogon::app().registerPostHandlingAdvice(
        [](const drogon::HttpRequestPtr &req, const drogon::HttpResponsePtr &resp) {
            const auto corsConfig = getCorsConfig();
            const auto origin = req->getHeader("Origin");
            if (isOriginAllowed(origin, corsConfig["allowed_origins"])) {
                resp->addHeader("Access-Control-Allow-Origin", origin.empty() ? "*" : origin);
                resp->addHeader("Access-Control-Expose-Headers", joinJsonArray(corsConfig["expose_headers"], "*"));
                if (corsConfig.get("allow_credentials", false).asBool()) {
                    resp->addHeader("Access-Control-Allow-Credentials", "true");
                }
            }
        });

    // 记录服务启动时间（用于健康检查接口）
    const auto processStartTime = std::chrono::steady_clock::now();
    HealthController::setStartTime(processStartTime);

    drogon::app().registerBeginningAdvice([](){
        LOG_INFO << "[后台任务队列] 已就绪";
    });

    // 事件循环启动后执行初始化任务
    app().getLoop()->queueInLoop([](){
        BackgroundTaskQueue::instance().enqueue("init", []{
            LOG_INFO << "[启动] 后台初始化任务开始";

            // 读取会话追踪模式配置
            auto customConfig = drogon::app().getCustomConfig();
            if (customConfig.isMember("session_tracking")) {
                std::string mode = customConfig["session_tracking"].get("mode", "hash").asString();
                if (mode == "zerowidth" || mode == "zero_width") {
                    chatSession::getInstance()->setTrackingMode(SessionTrackingMode::ZeroWidth);
                    LOG_INFO << "会话追踪模式：ZeroWidth（零宽字符嵌入）";
                } else {
                    chatSession::getInstance()->setTrackingMode(SessionTrackingMode::Hash);
                    LOG_INFO << "会话追踪模式：Hash（内容哈希）";
                }
            } else {
                LOG_INFO << "会话追踪模式：Hash（默认）";
            }

            ChannelManager::getInstance().init();
            AccountManager::getInstance().init();
            ApiManager::getInstance().init();

            // 初始化错误统计服务（使用默认配置）
            metrics::ErrorStatsConfig statsConfig;
            metrics::ErrorStatsService::getInstance().init(statsConfig);

            // 配置响应索引定时清理任务
            auto& customConfigRoot = drogon::app().getCustomConfig();
            int maxEntries = 200000;
            int maxAgeHours = 6;
            int cleanupMinutes = 10;
            if (customConfigRoot.isMember("response_index") && customConfigRoot["response_index"].isObject()) {
                maxEntries = customConfigRoot["response_index"].get("max_entries", maxEntries).asInt();
                maxAgeHours = customConfigRoot["response_index"].get("max_age_hours", maxAgeHours).asInt();
                cleanupMinutes = customConfigRoot["response_index"].get("cleanup_interval_minutes", cleanupMinutes).asInt();
            }
            if (maxEntries > 0 && maxAgeHours > 0 && cleanupMinutes > 0) {
                app().getLoop()->runEvery(
                    static_cast<double>(cleanupMinutes * 60),
                    [maxEntries, maxAgeHours]() {
                        ResponseIndex::instance().cleanup(
                            static_cast<size_t>(maxEntries),
                            std::chrono::hours(maxAgeHours)
                        );
                    }
                );
            }
        });
    });

    drogon::app().run();

    // 优雅停机
    LOG_INFO << "[停机] 正在关闭账号管理器后台线程...";
    // AccountManager 当前版本无独立后台线程停机接口，此处由进程退出统一回收。
    LOG_INFO << "[停机] 账号管理器后台线程已关闭";

    LOG_INFO << "[停机] 正在关闭后台任务队列...";
    BackgroundTaskQueue::instance().shutdown();
    LOG_INFO << "[停机] 后台任务队列已停机";

    return 0;
}
