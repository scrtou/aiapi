#ifndef CONTINUITY_RESOLVER_H
#define CONTINUITY_RESOLVER_H

#include <string>
#include <application/generation/contracts/GenerationRequest.h>
#include <application/generation/contracts/LegacySessionData.h>
#include <domain/port/IResponseIndex.h>
/**
 * @brief 会话连续性决策结果
 */
struct ContinuityDecision {
    enum class Source {
        PreviousResponseId,  // Responses：previous_response_id 命中
        ClientSession,       // Codex thread-id/session-id 稳定会话
        ZeroWidth,           // 零宽字符解析命中
        Hash,                // Hash 规则
        NewSession           // 创建新会话
    };

    Source source = Source::NewSession;
    SessionTrackingMode mode = SessionTrackingMode::Hash;
    std::string sessionId;
    std::string debug;
};

/**
 * @brief 会话连续性决策器
 *
 * 只负责“从请求得到应该使用的 sessionId”，不直接编排 provider 调用。
 */
class ContinuityResolver {
public:
    ContinuityResolver(IResponseIndex& responseIndex, SessionTrackingMode mode)
        : responseIndex_(responseIndex), mode_(mode) {}

    ContinuityDecision resolve(const GenerationRequest& req) const;

    static std::string generateNewSessionId();  // 生成格式：sess_<时间戳>_<随机串>

private:
    static std::string resolveHashSessionId(const GenerationRequest& req);
    static std::string resolveZeroWidthSessionId(const GenerationRequest& req);
    IResponseIndex& responseIndex_;
    SessionTrackingMode mode_;
};

#endif // 头文件保护结束
