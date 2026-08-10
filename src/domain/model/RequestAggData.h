#ifndef DOMAIN_MODEL_REQUEST_AGG_DATA_H
#define DOMAIN_MODEL_REQUEST_AGG_DATA_H

#include <chrono>
#include <string>

namespace metrics {

/**
 * @brief 请求聚合数据结构体
 *
 * 原定义在 dbManager/metrics/ErrorStatsDbManager.h。
 * 因 IErrorStatsSink 端口的 upsertRequestAggHour 需引用该类型，
 * 若留在 dbManager 层会把实现层拖进 domain，故上移至 domain/model。
 * 命名空间保持 metrics 不变，既有调用表达式无需改写。
 */
struct RequestAggData {
    std::chrono::system_clock::time_point ts;
    std::string provider;
    std::string model;
    std::string clientType;
    std::string apiKind;
    bool stream = false;
    int httpStatus = 0;
};

}  // namespace metrics

#endif  // DOMAIN_MODEL_REQUEST_AGG_DATA_H
