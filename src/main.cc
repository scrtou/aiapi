#include <drogon/drogon.h>
#include <runtime/AppContext.h>
#include <runtime/AppWiring.h>
#include <infrastructure/config/ConfigValidator.h>
#include <transport/controllers/AdminAuthFilter.h>
#include <transport/controllers/ClaudeRateLimitFilter.h>
#include <transport/controllers/RateLimitFilter.h>
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

    // 应用内所有必需 Store 都以此名字从 Drogon 取得 DB client。Drogon 在
    // getDbClient() 查询不存在的名字时会把空指针插入内部 map；随后失败路径
    // 调用 app().quit() 销毁该 map 会解引用空指针。这里在 run() 前 fail-fast，
    // 同时把「配置不完整」和「数据库实际不可用」清楚地区分开。
    bool hasRequiredDbClient = false;
    if (root["db_clients"].isArray()) {
        for (const auto& client : root["db_clients"]) {
            if (client.isObject() && client.get("name", "").asString() == "aichatpg") {
                hasRequiredDbClient = true;
                break;
            }
        }
    }
    if (!hasRequiredDbClient) {
        LOG_ERROR << "[配置校验]db_clients 必须配置 name=aichatpg 的数据库客户端";
    }

    return validation.valid && hasRequiredDbClient;
}

const Json::Value& getCustomConfig() {
    return drogon::app().getCustomConfig();
}

void ensureFilterReflectionRegistration() {
    // 显式触发过滤器反射注册，避免仅通过字符串路由引用时被链接器裁剪。
    (void)AdminAuthFilter::classTypeName();
    (void)ClaudeRateLimitFilter::classTypeName();
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

    // ---- 启动接线（P4-W2）----
    // Drogon 直到 app().run() 内部才会根据 db_clients 创建 DbClient。因而不能
    // 在调用 run() 前访问任何 DbManager；那会得到空 client，并把一个本应正常的
    // 配置误判为启动失败。BeginningAdvice 在 DbClient 创建之后、监听 socket 开放
    // 之前执行，正好提供所需的启动屏障：外部请求仍无法观察到半初始化服务。
    //
    // 仍然只在主 event loop 同步 build()，而不是退回 queueInLoop + 后台队列的
    // 双层 lambda。这样 StartupResult 仍被明确处理，失败会回滚已经启动的 owner，
    // 并立即进入 Drogon 的 shutdown，而不会以半启动状态持续对外服务。
    lifecycle::AppContext appContext;
    lifecycle::registerApplicationSteps(
        appContext, drogon::app().getCustomConfig(), processStartTime);

    enum class StartupState { Pending, Succeeded, Failed };
    StartupState startupState = StartupState::Pending;

    drogon::app().registerBeginningAdvice([&appContext, &startupState]() {
        const auto startup = appContext.build();
        if (!startup.canProceed()) {
            // stepsCompleted() 是已跑完的步骤数，失败时恰为失败步骤的 0-based 下标。
            LOG_FATAL << "[启动] 第 " << appContext.stepsCompleted()
                      << " 号步骤失败 code=" << lifecycle::toString(startup.error())
                      << " detail=" << startup.detail()
                      << "，已回滚 " << appContext.ownersStarted()
                      << " 个已启动 owner，终止启动";
            startupState = StartupState::Failed;
            // BeginningAdvice 运行在 main event loop；quit() 会在本轮 advice
            // 返回后停止 listener/DB/IO loop，避免短暂暴露一个失败的服务。
            drogon::app().quit();
            return;
        }

        // 降级不阻塞启动，但必须留痕：G8 的两处有意降级正是靠这里从「静默」变成「可见」。
        for (const auto& reason : appContext.degradedReasons()) {
            LOG_WARN << "[启动·降级] " << reason;
        }
        startupState = StartupState::Succeeded;
        LOG_INFO << "[启动] 接线完成：" << appContext.stepsCompleted() << " 个步骤，"
                 << appContext.ownersStarted() << " 个后台 owner";
    });

    // BeginningAdvice 按注册顺序运行；只有上一个启动屏障完成后才声明 HTTP 就绪。
    drogon::app().registerBeginningAdvice([&startupState](){
        if (startupState == StartupState::Succeeded) {
            LOG_INFO << "[启动] HTTP 监听已就绪，开始受理请求";
        }
    });

    drogon::app().run();

    if (startupState != StartupState::Succeeded) {
        // build() 失败时已做过回滚；shutdown() 的幂等性让这里同时覆盖
        // 「Drogon 尚未执行 beginning advice 就收到终止信号」这一极端路径。
        appContext.shutdown(std::chrono::steady_clock::now() +
                            std::chrono::seconds(kShutdownGraceSeconds));
        return 1;
    }

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
