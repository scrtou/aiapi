#ifndef SESSION_CODEC_H
#define SESSION_CODEC_H

#include <json/json.h>
#include <application/generation/contracts/GenerationSession.h>

/**
 * @brief session_st <-> Json 编解码（会话持久化快照）
 *
 * 设计约束：
 * - 持久状态可逆：RequestData / ResponseData / SessionState / ProviderContext 的
 *   durable 字段全部覆盖；仅本轮 materialize 使用的 recovery hint 不落盘。
 *   缺持久字段会在重启恢复后表现为「上下文丢失」或「路由到错误的上游会话」。
 * - 容错解码：缺失键使用 session_st 默认值，支持持久化 schema 演进。
 * - 纯函数、无 IO：便于单元测试直接做 encode->decode 往返校验。
 */
namespace sessioncodec {

/// 将会话快照序列化为 JSON（用于 SessionRow::payload）
Json::Value encodeSession(const session_st& session);

/// 从 JSON 快照还原会话；未知/缺失字段使用默认值。
session_st decodeSession(const Json::Value& payload);

} // namespace sessioncodec

#endif // 头文件保护结束
