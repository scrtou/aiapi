#ifndef CONTINUITY_RESOLVER_H
#define CONTINUITY_RESOLVER_H

#include <string>

#include "GenerationRequest.h"
#include "Session.h"

/**
 * @brief 会话连续性决策结果
 */
struct ContinuityDecision {
    enum class Source {
        PreviousResponseId,  // /v1/responses: previous_response_id 命中
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
    ContinuityDecision resolve(const GenerationRequest& req) const;

    static std::string generateNewSessionId();  // sess_<timestamp>_<rand>

private:
    static std::string resolveHashSessionId(const GenerationRequest& req);
    static std::string resolveZeroWidthSessionId(const GenerationRequest& req);
};

#endif // CONTINUITY_RESOLVER_H

