#include <drogon/drogon.h>
#include <accountManager/accountManager.h>
// R4 试点 C：main.cc 是组装根，需同时见到端口与实现才能接线。
// accountManager.h 已不再 include 实现头，故这里必须显式引入（IWYU）。
#include <dbManager/account/accountDbManager.h>
#include <apiManager/ApiManager.h>
#include <channelManager/channelManager.h>
#include <sessionManager/core/Session.h>
#include <metrics/ErrorStatsService.h>
#include <dbManager/channel/channelDbManager.h>
#include <retoolWorkspace/RetoolWorkspaceManager.h>
#include <dbManager/retoolWorkspace/RetoolWorkspaceDbManager.h>
#include <utils/BackgroundTaskQueue.h>
#include <utils/ApplicationShutdown.h>
#include <utils/ConfigValidator.h>
#include <sessionManager/continuity/ResponseIndex.h>
#include <dbManager/session/SessionDbManager.h>
#include <dbManager/chaynsThread/chaynsThreadDbManager.h>
#include <apipoint/chaynsapi/chaynsThreadReaper.h>
#include <controllers/HealthController.h>
#include <controllers/AdminAuthFilter.h>
#include <controllers/RateLimitFilter.h>
#include <algorithm>
#include <chrono>
#include <execinfo.h>
#include <fstream>
#include <iostream>
#include <exception>

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
    std::set_terminate([]() {
        auto current = std::current_exception();
        if (current) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& ex) {
                LOG_ERROR << "[terminate] uncaught exception: " << ex.what();
                std::cerr << "[terminate] uncaught exception: " << ex.what() << std::endl;
            } catch (...) {
                LOG_ERROR << "[terminate] uncaught non-std exception";
                std::cerr << "[terminate] uncaught non-std exception" << std::endl;
            }
        }
        void* frames[64];
        int n = backtrace(frames, 64);
        char** symbols = backtrace_symbols(frames, n);
        std::cerr << "[terminate] backtrace:" << std::endl;
        for (int i = 0; i < n; ++i) {
            std::cerr << "  " << symbols[i] << std::endl;
        }
        free(symbols);
        std::_Exit(1);
    });

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

    app().getLoop()->queueInLoop([](){
        const auto& customConfig = drogon::app().getCustomConfig();
        int requestedWorkerThreads = static_cast<int>(
            BackgroundTaskQueue::kDefaultWorkerThreads);
        if (customConfig.isMember("background_task_threads")) {
            if (customConfig["background_task_threads"].isIntegral()) {
                requestedWorkerThreads = customConfig["background_task_threads"].asInt();
            } else {
                LOG_WARN << "[后台任务队列] background_task_threads 不是整数，使用默认值 "
                         << BackgroundTaskQueue::kDefaultWorkerThreads;
            }
        }
        const int workerThreads = std::clamp(requestedWorkerThreads, 2, 64);
        if (workerThreads != requestedWorkerThreads) {
            LOG_WARN << "[后台任务队列] background_task_threads="
                     << requestedWorkerThreads << " 超出 2-64 范围，已调整为 "
                     << workerThreads;
        }
        BackgroundTaskQueue::instance().start(static_cast<size_t>(workerThreads));

        BackgroundTaskQueue::instance().enqueue("init", []{
            LOG_INFO << "[启动] 后台初始化任务开始";

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

            // R4 试点 B：注入 Channel 持久化实现。
            // 必须早于 init()——init() 会立刻建表并写入内置渠道。
            ChannelManager::getInstance().setStore(ChannelDbManager::getInstance());
            ChannelManager::getInstance().init();
            // R4 试点 C：注入 Account 持久化实现。
            // 必须早于 init()——init() 会立刻建表并迁移/规整既有账号。
            AccountManager::getInstance().setStore(AccountDbManager::getInstance());
            // /ready 的库探针复用同一个 AccountDbManager 实例（未新造端口）。
            // 漏注入不会崩溃，只会让 /ready 恒报 not_ready —— 故由启动接线门禁守住。
            HealthController::setDbProbe(AccountDbManager::getInstance());
            // 渠道列表来源，同样必须早于 init()：init() 启动的后台线程会调用
            // checkChannelAccountCounts()。未注入则回退 Null 实现、渠道列表恒空，
            // 自动补注册静默失效（不崩溃，故必须由启动接线门禁守住）。
            AccountManager::getInstance().setChannelStore(ChannelDbManager::getInstance());
            AccountManager::getInstance().init();
            RetoolWorkspaceManager::getInstance().setStore(RetoolWorkspaceDbManager::getInstance());
            RetoolWorkspaceManager::getInstance().init();
            ApiManager::getInstance().init();

            metrics::ErrorStatsConfig statsConfig;
            metrics::ErrorStatsService::getInstance().init(statsConfig);

            // ---- 会话持久化接线：建表成功后才开启写穿/懒加载 ----
            // 说明：ResponseIndex 与后续 session_map 的写穿逻辑均以 persistenceEnabled 为总开关，
            //       建表失败时静默降级为纯内存，绝不阻塞请求链路。
            {
                auto sessionDb = SessionDbManager::getInstance();
                std::string dbErr;
                if (sessionDb->ensureTables(&dbErr)) {
                    sessionDb->setEnabled(true);
                    ResponseIndex::instance().setPersistenceEnabled(true);
                    // 会话状态写穿/懒加载与 ResponseIndex 同生命周期开启，
                    // 保证 responseId->sessionId 索引与会话快照要么都持久化、要么都不持久化。
                    chatSession::getInstance()->setPersistenceEnabled(true);
                    LOG_INFO << "[会话持久化] 已启用：chat_session_state / response_index 写穿与懒加载生效";
                } else {
                    sessionDb->setEnabled(false);
                    ResponseIndex::instance().setPersistenceEnabled(false);
                    chatSession::getInstance()->setPersistenceEnabled(false);
                    LOG_WARN << "[会话持久化] 未启用，降级为纯内存会话：" << dbErr;
                }
            }

            // ---- chayns 上游线程台账接线：与会话持久化独立开关 ----
            // 台账解决的是"上游 thread 泄漏"：内存 m_threadMap 随进程消失，
            // 而上游 thread 是远端资源，只能靠这张表 + reaper 回收。
            // 建表失败时整套机制静默关闭，chayns 正常聊天不受影响。
            {
                auto threadDb = chaynsThreadDbManager::getInstance();
                std::string threadDbErr;
                bool threadLedgerEnabled = false;
                if (customConfig.isMember("chayns_thread_reaper") &&
                    customConfig["chayns_thread_reaper"].isObject()) {
                    threadLedgerEnabled =
                        customConfig["chayns_thread_reaper"].get("enabled", true).asBool();
                } else {
                    threadLedgerEnabled = true;  // 未配置时默认开启，避免静默泄漏
                }
                if (!threadLedgerEnabled) {
                    threadDb->setEnabled(false);
                    LOG_WARN << "[chayns线程台账] 已按配置关闭，上游 thread 将不再被回收";
                } else if (threadDb->ensureTable(&threadDbErr)) {
                    threadDb->setEnabled(true);
                    LOG_INFO << "[chayns线程台账] 已启用：chaynsa_thread 写穿生效";

                    // 台账可用才启动回收器：表都建不出来时启动它只会空转刷日志。
                    // 配置单位对齐运维直觉——间隔用分钟、空闲用小时，内部换算成秒。
                    chaynsThreadReaper::Options reaperOpt;
                    if (customConfig["chayns_thread_reaper"].isObject()) {
                        const Json::Value& rc = customConfig["chayns_thread_reaper"];
                        reaperOpt.scanIntervalSeconds = static_cast<int>(
                            rc.get("scan_interval_minutes", 15).asDouble() * 60);
                        reaperOpt.idleSeconds = static_cast<int>(
                            rc.get("idle_hours", 24).asDouble() * 3600);
                        reaperOpt.batchLimit      = rc.get("batch_limit", 50).asInt();
                        reaperOpt.maxAttempts     = rc.get("max_attempts", 5).asInt();
                        reaperOpt.deleteSpacingMs = rc.get("delete_spacing_ms", 200).asInt();
                    }
                    chaynsThreadReaper::getInstance().start(reaperOpt);
                } else {
                    threadDb->setEnabled(false);
                    LOG_WARN << "[chayns线程台账] 未启用，上游 thread 不会被回收：" << threadDbErr;
                }
            }

            // ---- 会话持久化可调参数（配置单位：小时）：内存TTL / 内存清理间隔 / DB保留期 / 两个落库开关 ----
            // 必须在 startClearExpiredSession() 之前应用，否则清理线程会先按默认值起跑。
            {
                // 配置单位为“小时”，内部统一换算为秒；允许小数（0.5 = 30 分钟），换算结果最小 1 秒。
                const auto hoursToSeconds = [](double hours) {
                    const double seconds = hours * 3600.0;
                    return seconds < 1.0 ? 1 : static_cast<int>(seconds + 0.5);
                };
                int  memExpire   = SESSION_EXPIRE_TIME;
                int  memInterval = SESSION_CLEANUP_INTERVAL;
                int  dbRetention = SESSION_EXPIRE_TIME;
                bool storePayload = true;
                bool storeBody    = false;
                if (customConfig.isMember("session_persistence") &&
                    customConfig["session_persistence"].isObject()) {
                    const auto& sp = customConfig["session_persistence"];
                    if (sp["memory_expire_hours"].isNumeric()) {
                        memExpire = hoursToSeconds(sp["memory_expire_hours"].asDouble());
                    }
                    if (sp["memory_cleanup_interval_hours"].isNumeric()) {
                        memInterval = hoursToSeconds(sp["memory_cleanup_interval_hours"].asDouble());
                    }
                    if (sp["db_retention_hours"].isNumeric()) {
                        dbRetention = hoursToSeconds(sp["db_retention_hours"].asDouble());
                    }
                    storePayload = sp.get("store_session_payload", storePayload).asBool();
                    storeBody    = sp.get("store_response_body", storeBody).asBool();
                }
                auto* cs = chatSession::getInstance();
                cs->setSessionExpireSeconds(memExpire);
                cs->setCleanupIntervalSeconds(memInterval);
                cs->setDbRetentionSeconds(dbRetention);
                cs->setStoreSessionPayload(storePayload);
                ResponseIndex::instance().setStoreResponseBody(storeBody);
                LOG_INFO << "[会话持久化] 参数生效: 内存TTL=" << cs->getSessionExpireSeconds() / 3600.0
                         << "h, 内存清理间隔=" << cs->getCleanupIntervalSeconds() / 3600.0
                         << "h, DB保留=" << cs->getDbRetentionSeconds() / 3600.0
                         << "h, payload落库=" << (cs->isStoreSessionPayloadEnabled() ? "on" : "off")
                         << ", response_body落库="
                         << (ResponseIndex::instance().isStoreResponseBodyEnabled() ? "on" : "off");
            }

            // ---- 内存会话清理线程：此前从未启动，导致 session_map 无限增长 ----
            chatSession::getInstance()->startClearExpiredSession();
            LOG_INFO << "[会话清理] 内存会话过期清理线程已启动";

            int maxEntries = 200000;
            int maxAgeHours = 6;
            int cleanupMinutes = 10;
            if (customConfig.isMember("response_index") && customConfig["response_index"].isObject()) {
                maxEntries = customConfig["response_index"].get("max_entries", maxEntries).asInt();
                maxAgeHours = customConfig["response_index"].get("max_age_hours", maxAgeHours).asInt();
                cleanupMinutes = customConfig["response_index"].get("cleanup_interval_minutes", cleanupMinutes).asInt();
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
    //
    // 顺序约束：Reaper 必须先于 BackgroundTaskQueue 停止，但理由不是依赖关系——
    // Reaper 只调用 chaynsThreadDbManager 的同步方法（loadThreadsOlderThan /
    // deleteThread / purgeExhaustedThreads），全程不碰 BackgroundTaskQueue，
    // 两者之间不存在生产者-消费者关系。
    //
    // 当前精确语义是串行 join，并不会与后续步骤重叠：Reaper 单轮可能对多个
    // 上游线程逐个发 HTTP DELETE，若正阻塞在同步 IO 中，stop() 会一直等到该
    // 调用返回。把它放在首位只保证其他 owner 尚未 teardown；它仍可能独占整个
    // SIGTERM 宽限期。P1 harness 已记录该缺口，deadline/cancellation 留到 P4。
    // N4: 原先 AccountManager 的 4 个后台线程全部 detach，进程退出时被强行
    // 截断，可能在持有 accountListNeedUpdateMutex 或 DB 连接的状态下消失。
    // 现已改为持有 std::thread 成员 + 条件变量可中断睡眠，此处统一 join。
    //
    // 顺序：必须在 BackgroundTaskQueue::shutdown() 之前——账号线程（尤其是
    // checkToken/cleanExpiredAccounts）会向该队列投递任务，若队列先关闭并
    // fail-fast 拒收，这些任务会静默丢失且刷屏拒收日志。
    // N4: 会话过期清理线程同样由 detach 改为 join；它在每轮里会同步删除 DB 中
    // 过期快照，必须在 DB 相关设施拆除前干净退出。
    lifecycle::runApplicationShutdown({
        [] { chaynsThreadReaper::getInstance().stop(); },
        [] { AccountManager::getInstance().stopBackgroundThreads(); },
        [] { chatSession::getInstance()->stopClearExpiredSession(); },
        [] { BackgroundTaskQueue::instance().shutdown(); },
    });

    return 0;
}
