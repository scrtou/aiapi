#include <drogon/drogon_test.h>

#include <domain/port/IErrorStatsSink.h>
#include <infrastructure/metrics/ErrorStatsService.h>
#include <infrastructure/metrics/ErrorStatsConfig.h>

#include <memory>
#include <string>
#include <vector>

// IErrorStatsSink 端口契约回归测试。
//
// 覆盖面刻意收窄：只验证 ErrorStatsService 与 IErrorStatsSink 之间的契约，
// 即「入队 -> flush -> 落到 sink」这条路径确实走的是注入的实现。
// 不覆盖 main.cc 的启动装配（本测试自行注入），也不覆盖 SQL 语义
// （那属于 ErrorStatsDbManager 自己的职责，需要真实库）。
//
// 承载力说明：aiapi_test 链接真实的 ErrorStatsService.cpp，不是桩。
// 谁把构造注入/flushEvents/flushRequestAgg 改坏，这个文件就会变红。
// 服务是测试局部对象，避免依赖跨用例的 singleton 初始化顺序。

namespace
{

class FakeErrorStatsSink : public metrics::IErrorStatsSink
{
  public:
    int initCalls = 0;
    int insertEventsCalls = 0;
    int upsertErrorAggCalls = 0;
    int upsertRequestAggCalls = 0;
    int cleanupEventsCalls = 0;
    int cleanupAggCalls = 0;

    int lastRetentionDaysDetail = -1;
    int lastRetentionDaysAgg = -1;

    std::vector<metrics::ErrorEvent> insertedEvents;
    std::vector<metrics::RequestAggData> requestAgg;

    void init() override { ++initCalls; }

    bool insertEvents(const std::vector<metrics::ErrorEvent>& events) override
    {
        ++insertEventsCalls;
        insertedEvents.insert(insertedEvents.end(), events.begin(), events.end());
        return true;
    }

    bool upsertErrorAggHour(const std::vector<metrics::ErrorEvent>&) override
    {
        ++upsertErrorAggCalls;
        return true;
    }

    bool upsertRequestAggHour(const metrics::RequestAggData& data) override
    {
        ++upsertRequestAggCalls;
        requestAgg.push_back(data);
        return true;
    }

    int cleanupOldEvents(int retentionDays) override
    {
        ++cleanupEventsCalls;
        lastRetentionDaysDetail = retentionDays;
        return 3;
    }

    int cleanupOldAgg(int retentionDays) override
    {
        ++cleanupAggCalls;
        lastRetentionDaysAgg = retentionDays;
        return 4;
    }
};

}  // namespace

DROGON_TEST(ErrorStatsSinkPortContract)
{
    auto fake = std::make_shared<FakeErrorStatsSink>();

    metrics::ErrorStatsService service(fake);

    metrics::ErrorStatsConfig config;
    config.enabled = true;
    config.persistDetail = true;
    config.persistAgg = true;
    config.persistRequestAgg = true;
    config.retentionDaysDetail = 7;
    config.retentionDaysAgg = 11;
    config.asyncBatchSize = 200;
    config.asyncFlushMs = 50;

    service.init(config);

    // init() 必须使用构造时注入的 sink；不存在 ErrorStatsDbManager singleton
    // fallback。若构造注入被绕过，此处即为 0。
    REQUIRE(fake->initCalls == 1);

    service.recordError(metrics::Domain::INTERNAL,
                        "internal.port_contract_probe",
                        "sink port regression probe",
                        "req-sink-port-1");

    metrics::RequestCompletedData completed;
    completed.provider = "probe-provider";
    completed.model = "probe-model";
    completed.clientType = "probe-client";
    completed.apiKind = "chat";
    completed.stream = true;
    completed.httpStatus = 200;
    service.recordRequestCompleted(completed);

    // flushNow 同步排空两条队列，不依赖后台线程的调度时机。
    service.flushNow();

    // 明细与错误聚合都必须落到注入的 sink。
    CHECK(fake->insertEventsCalls >= 1);
    CHECK(fake->upsertErrorAggCalls >= 1);

    bool foundProbe = false;
    for (const auto& ev : fake->insertedEvents)
    {
        if (ev.requestId == "req-sink-port-1")
        {
            foundProbe = true;
            break;
        }
    }
    CHECK(foundProbe);

    // 请求聚合的字段搬运必须无损（RequestCompletedData -> RequestAggData）。
    REQUIRE(fake->upsertRequestAggCalls >= 1);
    bool foundAgg = false;
    for (const auto& agg : fake->requestAgg)
    {
        if (agg.provider == "probe-provider" && agg.model == "probe-model")
        {
            foundAgg = true;
            CHECK(agg.clientType == "probe-client");
            CHECK(agg.apiKind == "chat");
            CHECK(agg.stream == true);
            CHECK(agg.httpStatus == 200);
            break;
        }
    }
    CHECK(foundAgg);

    // runCleanup 必须把配置里的保留天数原样透传给端口，而不是用硬编码默认值。
    int cleaned = service.runCleanup();
    CHECK(cleaned == 7);  // 3 + 4，由 Fake 约定
    CHECK(fake->lastRetentionDaysDetail == 7);
    CHECK(fake->lastRetentionDaysAgg == 11);

    service.shutdown();
}
