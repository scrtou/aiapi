#ifndef CONTINUITY_RESOLVER_H
#define CONTINUITY_RESOLVER_H

#include <string>
#include "sessionManager/contracts/GenerationRequest.h"
#include "sessionManager/core/Session.h"
/**
 * @brief 会话连续性决策结果
 */
struct ContinuityDecision {
    enum class Source {
        PreviousResponseId,  // // Responses： previous_响应_id 命中
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

    static std::string generateNewSessionId();  // 生成格式：sess_<时间戳>_<随机串>

private:
    static std::string resolveHashSessionId(const GenerationRequest& req);
    static std::string resolveZeroWidthSessionId(const GenerationRequest& req);
};

#endif // 头文件保护结束

