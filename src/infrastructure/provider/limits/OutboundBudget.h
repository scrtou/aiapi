#ifndef OUTBOUND_BUDGET_H
#define OUTBOUND_BUDGET_H

#include <json/json.h>
#include <cstddef>
#include <string>
#include <vector>

namespace continuity {

// 每个 Provider 的出站请求上限互相独立：上游实现不同，硬限也不同。
// 读取顺序：custom_config.outbound_limits.<provider>.max_request_bytes
//        -> custom_config.outbound_limits.default.max_request_bytes
//        -> 内置兜底值。
// 返回 0 表示不限制。
size_t outboundMaxRequestBytes(const std::string& providerKey);

// 单条消息在该 Provider 下的上限（用于 retool 这类逐条回放的上游）。
size_t outboundMaxMessageBytes(const std::string& providerKey);

// 渐进退化档位：返回历史预算的递减序列（分母 1,2,4,... 直到 0）。
// 例如 base=800000 -> {800000, 400000, 200000, 100000, 0}
std::vector<size_t> degradationLadder(size_t baseBudget, size_t steps = 4);

// 出站门禁判定结果。
struct OutboundCheck {
    bool withinLimit = true;
    size_t actualBytes = 0;
    size_t limitBytes = 0;
};

OutboundCheck checkOutboundSize(const std::string& providerKey, size_t bodyBytes);
OutboundCheck checkOutboundSize(const std::string& providerKey, const Json::Value& body);

// 把 Json 以紧凑格式序列化后的字节数（与实际发送体一致的度量口径）。
size_t compactBodyBytes(const Json::Value& body);

}  // namespace continuity

#endif
