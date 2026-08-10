#include <drogon/drogon.h>
#include <accountManager/accountManager.h>
#include <accountManager/RetoolProvisionClock.h>
// R4 试点 C：main.cc 是组装根，需同时见到端口与实现才能接线。
// accountManager.h 已不再 include 实现头，故这里必须显式引入（IWYU）。
#include <dbManager/account/accountDbManager.h>
#include <apiManager/ApiManager.h>
#include <channelManager/channelManager.h>
#include <sessionManager/core/Session.h>
#include <dbManager/metrics/ErrorStatsDbManager.h>
#include <metrics/ErrorStatsService.h>
#include <dbManager/channel/channelDbManager.h>
#include <retoolWorkspace/RetoolWorkspaceManager.h>
#include <dbManager/retoolWorkspace/RetoolWorkspaceDbManager.h>
#include <runtime/AppContext.h>
#include <runtime/AppWiring.h>
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

// SIGTERM 到 SIGKILL 的宽限期。取 25s 是为了落在容器编排默认的 30s
// terminationGracePeriod 之内，留 5s 余量给进程收尾与日志刷盘。
constexpr int kShutdownGraceSeconds = 25;

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

    // 这里只宣告「HTTP 开始受理」。后台队列的就绪与否不再由它表达——队列在
    // build() 里已于 run() 之前同步启动，若在此打印「队列已就绪」，日志时序会
    // 晚于事实，排查启动顺序问题时反而误导。
    drogon::app().registerBeginningAdvice([](){
        LOG_INFO << "[启动] HTTP 监听已就绪，开始受理请求";
    });

    // ---- 启动接线（P4-W2）----
    // 从前的写法是把整段初始化塞进 queueInLoop + BackgroundTaskQueue::enqueue 的
    // 双层 lambda。代价有三：其一，enqueue 失败时那句 `return 1` 返回给的是
    // trantor 的 std::function<void()>，返回值被丢弃，进程带着未建表的 Store
    // 继续服务；其二，初始化跑在队列 worker 上，与 run() 并发，谁先谁后取决于
    // 调度；其三，中途失败没有回滚，已起的线程留在原地。
    //
    // 改为在 main 线程上同步 build()：失败即 return，且 AppContext 按登记逆序
    // 回滚已启动的 owner。run() 之后的停机也复用同一份 owner 列表，不再有
    // 「启动登记一处、停机硬编码另一处」的双份真相。
    lifecycle::AppContext appContext;
    lifecycle::registerApplicationSteps(appContext, drogon::app().getCustomConfig());

    const auto startup = appContext.build();
    if (!startup.canProceed()) {
        // stepsCompleted() 是已跑完的步骤数，失败时恰为失败步骤的 0-based 下标。
        LOG_FATAL << "[启动] 第 " << appContext.stepsCompleted()
                  << " 号步骤失败 code=" << lifecycle::toString(startup.error())
                  << " detail=" << startup.detail()
                  << "，已回滚 " << appContext.ownersStarted()
                  << " 个已启动 owner，终止启动";
        return 1;
    }
    // 降级不阻塞启动，但必须留痕：G8 的两处有意降级正是靠这里从「静默」变成「可见」。
    for (const auto& reason : appContext.degradedReasons()) {
        LOG_WARN << "[启动·降级] " << reason;
    }
    LOG_INFO << "[启动] 接线完成：" << appContext.stepsCompleted() << " 个步骤，"
             << appContext.ownersStarted() << " 个后台 owner";

    drogon::app().run();

    // 优雅停机：顺序不再写在这里。
    //
    // 从前此处硬编码 reaper -> account -> session -> queue 四元组，与启动处的
    // 登记顺序是两份彼此独立、需人工同步的真相；新增一个后台 owner 而忘了改这里，
    // 症状是进程退出时线程被强杀，且不会有任何编译期提示。
    //
    // 现在 AppContext 按登记逆序停机，登记又紧跟各自的启动，故顺序由构造过程本身
    // 保证。唯一的真实约束——三个 owner 都必须早于 BackgroundTaskQueue 停止
    // （账号线程与会话清理线程会向队列投递任务，队列先关会 fail-fast 拒收并丢任务）
    // ——因队列是第一个登记、最后一个停止而自动满足。
    //
    // deadline 取绝对时间点而非给每段一份相对超时：停机跨多个 owner，相对超时会
    // 逐段累加，总时长突破 SIGTERM 宽限期。
    // 已知缺口：reaper 单轮可能阻塞在同步 HTTP DELETE 上，其 stop() 会等该调用
    // 返回，deadline 只能限制「还剩多少」的计算，无法真正打断它。
    appContext.shutdown(std::chrono::steady_clock::now() +
                        std::chrono::seconds(kShutdownGraceSeconds));

    return 0;
}
