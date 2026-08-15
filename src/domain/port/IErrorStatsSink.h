#ifndef DOMAIN_PORT_IERROR_STATS_SINK_H
#define DOMAIN_PORT_IERROR_STATS_SINK_H

#include <domain/model/ErrorEvent.h>
#include <domain/model/RequestAggData.h>
#include <vector>

namespace metrics {

// 错误统计落库端口（依赖倒置）。
// 上层 infrastructure/metrics/ErrorStatsService 只依赖本接口，不再依赖 ErrorStatsDbManager 具体实现。
// 方法签名逐字取自 ErrorStatsDbManager，因此既有调用表达式无需改写。
//
// 成员取舍依据 ErrorStatsService 的实测调用点：
//   init / insertEvents / upsertErrorAggHour / upsertRequestAggHour /
//   cleanupOldEvents / cleanupOldAgg 六个方法均有调用；
//   queryErrorSeries / queryRequestSeries / queryEvents / queryEventById
//   仅被查询侧（controller）直接经由 DbManager 调用，Service 从不使用，故不纳入；
//   getDbType() 返回 DbType —— 属 dbManager 实现细节，纳入会把该枚举拖进 domain。
class IErrorStatsSink
{
  public:
    virtual ~IErrorStatsSink() = default;

    virtual void init() = 0;

    virtual bool insertEvents(const std::vector<ErrorEvent>& events) = 0;
    virtual bool upsertErrorAggHour(const std::vector<ErrorEvent>& events) = 0;
    virtual bool upsertRequestAggHour(const RequestAggData& data) = 0;

    virtual int cleanupOldEvents(int retentionDays) = 0;
    virtual int cleanupOldAgg(int retentionDays) = 0;
};

}  // namespace metrics

#endif  // DOMAIN_PORT_IERROR_STATS_SINK_H
