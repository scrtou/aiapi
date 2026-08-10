#include <drogon/drogon_test.h>

#include <runtime/StartupResult.h>

#include <string>

using lifecycle::StartupError;
using lifecycle::StartupResult;
using lifecycle::toString;

/**
 * 原因码的意义在于「运维读到码就知道该做什么」，所以 toString 必须逐个可辨且
 * 互不相同。用穷举 + 去重断言，避免有人新增枚举项时复制粘贴漏改字符串——
 * 那种错误编译期无感，只在事故当天的日志里表现为两种故障同名。
 */
DROGON_TEST(StartupErrorToStringIsTotalAndDistinct)
{
    const StartupError all[] = {
        StartupError::None,
        StartupError::ConfigInvalid,
        StartupError::ExecutorRejected,
        StartupError::StoreInitFailed,
        StartupError::OwnerStartFailed,
        StartupError::AlreadyBuilt,
    };

    for (const auto e : all) {
        const std::string name = toString(e);
        CHECK(!name.empty());
        CHECK(name != "Unknown");
    }

    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(all) / sizeof(all[0]); ++j) {
            CHECK(std::string(toString(all[i])) != std::string(toString(all[j])));
        }
    }
}

/// 越界值不得走进未定义行为，必须落到 Unknown 兜底分支。
DROGON_TEST(StartupErrorToStringHandlesOutOfRange)
{
    const auto bogus = static_cast<StartupError>(9999);
    CHECK(std::string(toString(bogus)) == "Unknown");
}

DROGON_TEST(StartupResultOkCarriesNoError)
{
    const auto r = StartupResult::ok();
    CHECK(r.isOk());
    CHECK(!r.isDegraded());
    CHECK(!r.isFailed());
    CHECK(r.canProceed());
    CHECK(r.error() == StartupError::None);
    CHECK(r.detail().empty());
    CHECK(std::string(r.stateName()) == "Ok");
}

/**
 * G8 的核心断言：会话快照表 / chayns 台账建表失败是**有意**的降级路径，
 * 进程必须继续启动。若日后有人把 Degraded 并入 Failed，本用例即刻变红——
 * 这正是要防的「fail-fast 误伤本可服务的进程」。
 */
DROGON_TEST(StartupResultDegradedStillProceedsAndKeepsDetail)
{
    const auto r = StartupResult::degraded("session snapshot table unavailable");
    CHECK(r.isDegraded());
    CHECK(!r.isOk());
    CHECK(!r.isFailed());
    CHECK(r.canProceed());
    CHECK(r.error() == StartupError::None);
    CHECK(r.detail() == "session snapshot table unavailable");
    CHECK(std::string(r.stateName()) == "Degraded");
}

/**
 * G1 的核心断言：执行器拒收初始化任务此前只打一条 LOG_FATAL 就继续跑。
 * 现在它必须是不可继续的终态，且原因码可从结果本身读出。
 */
DROGON_TEST(StartupResultFailedBlocksProceedAndKeepsCode)
{
    const auto r = StartupResult::failed(StartupError::ExecutorRejected,
                                         "enqueue(init) returned ShuttingDown");
    CHECK(r.isFailed());
    CHECK(!r.canProceed());
    CHECK(r.error() == StartupError::ExecutorRejected);
    CHECK(r.detail() == "enqueue(init) returned ShuttingDown");
    CHECK(std::string(r.stateName()) == "Failed");
}

/// 每个失败原因码都应能被完整构造并原样取回，防止构造函数丢字段。
DROGON_TEST(StartupResultFailedRoundTripsEveryCode)
{
    const StartupError codes[] = {
        StartupError::ConfigInvalid,
        StartupError::ExecutorRejected,
        StartupError::StoreInitFailed,
        StartupError::OwnerStartFailed,
        StartupError::AlreadyBuilt,
    };

    for (const auto code : codes) {
        const std::string detail = std::string("detail-") + toString(code);
        const auto r = StartupResult::failed(code, detail);
        CHECK(r.isFailed());
        CHECK(!r.canProceed());
        CHECK(r.error() == code);
        CHECK(r.detail() == detail);
    }
}
