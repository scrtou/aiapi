#pragma once

#include <runtime/AppContext.h>

#include <json/json.h>

#include <chrono>

namespace lifecycle {

/**
 * @brief 把 main.cc 里那段 200 余行的初始化流程，改写成向 AppContext 注册的步骤序列。
 *
 * 为什么要单独成文件而不是留在 main.cc：main.cc 不进任何测试目标（它自带
 * `int main`），留在那里的接线逻辑永远只能靠「启动一次看日志」来验证。搬到
 * 这里之后，注册出来的步骤表本身成了可断言的数据——顺序、名字、哪些步骤在
 * 配置缺失时降级而非失败，都能在不建库、不起线程的前提下检查。
 *
 * 本函数只做**注册**，不执行任何步骤；真正的执行发生在 AppContext::build()，
 * 且在其调用线程上同步完成（G2：不再横跨 main 线程 → event loop → 队列 worker）。
 *
 * @param ctx           待填充的 composition root。
 * @param customConfig  drogon 的 custom_config 子树；显式传入而非在步骤内部
 *                      调用 drogon::app()，使步骤可在无 drogon 实例时被测试。
 * @param processStartTime  main 在构建前记录的进程启动时刻；Health use case
 *                          以它计算 uptime，而不直接读取 Controller 静态状态。
 */
void registerApplicationSteps(
    AppContext& ctx, const Json::Value& customConfig,
    std::chrono::steady_clock::time_point processStartTime);

// ---------------------------------------------------------------------------
// 配置解析的纯函数接缝。它们此前是埋在 lambda 里的内联表达式，无法单测；
// 而恰恰是这些「越界怎么办、类型不对怎么办」的分支最容易在改配置时出事。
// ---------------------------------------------------------------------------

/// 后台队列 worker 线程数：非整数回退默认值，越界钳到 [2, 64]。
int resolveWorkerThreads(const Json::Value& customConfig);

/// 小时 → 秒，允许小数（0.5 = 30 分钟），换算结果下限 1 秒。
int hoursToSeconds(double hours);

/// chayns 台账/回收器是否启用：未配置时默认 true（避免上游 thread 静默泄漏）。
bool resolveThreadLedgerEnabled(const Json::Value& customConfig);

}  // namespace lifecycle
