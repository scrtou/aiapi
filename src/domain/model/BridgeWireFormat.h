#ifndef DOMAIN_MODEL_BRIDGEWIREFORMAT_H
#define DOMAIN_MODEL_BRIDGEWIREFORMAT_H

namespace toolcall {

/**
 * @brief 工具桥接线格式（纯枚举，无行为）
 *
 * 从 application/generation/tooling/BridgeProtocolCodec.h 抽出。该 codec 头还依赖
 * ActionProtocolCompiler.h 与 GenerationEvent.h，而 session_st 只需要这个枚举。
 * 抽出后 Session.h 不必再拖入整条 codec 依赖链。
 */
enum class BridgeWireFormat {
    Unset,
    Json,
    Xml
};

} // namespace toolcall

#endif // DOMAIN_MODEL_BRIDGEWIREFORMAT_H
