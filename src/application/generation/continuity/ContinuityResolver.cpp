#include <application/generation/continuity/ContinuityResolver.h>
#include <application/generation/continuity/TextExtractor.h>
#include <platform/ZeroWidthEncoder.h>
#include <chrono>
#include <random>
#include <sstream>
#include <application/generation/core/Session.h>

namespace {

std::string roleToString(MessageRole role) {
    switch (role) {
        case MessageRole::System: return "system";
        case MessageRole::User: return "user";
        case MessageRole::Assistant: return "assistant";
        case MessageRole::Tool: return "tool";
        default: return "user";
    }
}

} // 命名空间结束

ContinuityDecision ContinuityResolver::resolve(const GenerationRequest& req) const {
    ContinuityDecision decision;
    decision.mode = mode_;

    // // Responses： previous_响应_id 优先
    if (req.usesStoredResponseLifecycle()) {
        if (req.previousResponseId.has_value() && !req.previousResponseId->empty()) {
            std::string sessionId;
            if (responseIndex_.tryGetSessionId(*req.previousResponseId, sessionId) &&
                !sessionId.empty()) {
                decision.source = ContinuityDecision::Source::PreviousResponseId;
                decision.sessionId = sessionId;
                decision.debug = "previous_response_id hit";
                return decision;
            }

            decision.source = ContinuityDecision::Source::NewSession;
            decision.sessionId = generateNewSessionId();
            decision.debug = "previous_response_id miss -> new session";
            return decision;
        }
    }

    // 只有当前请求确实携带稳定客户端会话 ID 时才绕过 ZeroWidth/Hash。
    // 内部主键加入调用方作用域，避免不同 API 用户使用相同客户端 ID 时串话。
    if (continuity::hasStableClientSession(req.clientInfo)) {
        Json::Value keyData(Json::objectValue);
        keyData["kind"] = "client_session";
        keyData["client_type"] = req.clientInfo.get("client_type", "").asString();
        keyData["session_id"] = req.clientInfo["client_session_id"].asString();

        std::string scope = req.clientInfo.get("client_authorization", "").asString();
        if (scope.empty()) {
            scope = req.clientInfo.get("workspace_id", "").asString();
        }
        keyData["scope"] = scope;

        decision.source = ContinuityDecision::Source::ClientSession;
        decision.sessionId = "codex_" + chatSession::generateConversationKey(keyData);
        decision.debug = "codex client session (" +
                         req.clientInfo.get("client_session_source", "unknown").asString() + ")";
        return decision;
    }

    // 无 previous_response_id：按配置模式决策
    if (mode_ == SessionTrackingMode::ZeroWidth) {
        const std::string sid = resolveZeroWidthSessionId(req);
        if (!sid.empty()) {
            decision.source = ContinuityDecision::Source::ZeroWidth;
            decision.sessionId = sid;
            decision.debug = "zerowidth decode hit";
            return decision;
        }

        decision.source = ContinuityDecision::Source::NewSession;
        decision.sessionId = generateNewSessionId();
        decision.debug = "zerowidth decode miss -> new session";
        return decision;
    }


    decision.source = ContinuityDecision::Source::Hash;
    decision.sessionId = resolveHashSessionId(req);
    decision.debug = "hash";
    if (decision.sessionId.empty()) {
        decision.source = ContinuityDecision::Source::NewSession;
        decision.sessionId = generateNewSessionId();
        decision.debug = "hash empty -> new session";
    }
    return decision;
}

std::string ContinuityResolver::generateNewSessionId() {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    return "sess_" + std::to_string(timestamp) + "_" + std::to_string(dis(gen));
}

std::string ContinuityResolver::resolveZeroWidthSessionId(const GenerationRequest& req) {
    const auto texts = TextExtractor::extractForContinuity(req);
    std::optional<std::string> last;

    for (const auto& t : texts) {
        if (t.empty()) continue;
        auto decoded = ZeroWidthEncoder::decode(t);
        if (decoded.has_value() && !decoded->empty()) {
            last = *decoded;
        }
    }

    return last.value_or("");
}

std::string ContinuityResolver::resolveHashSessionId(const GenerationRequest& req) {
    // 稳定键规则：keyData = {， 客户端_信息，模型 } -> SHA256(Styled写入r JSON)
    Json::Value keyData(Json::objectValue);
    Json::Value messages(Json::arrayValue);

    for (const auto& msg : req.messages) {
        Json::Value m(Json::objectValue);
        m["role"] = roleToString(msg.role);
        m["content"] = msg.getTextContent();
        messages.append(m);
    }

    keyData["messages"] = messages;
    keyData["clientInfo"] = req.clientInfo;
    keyData["model"] = req.model;

    return chatSession::generateConversationKey(keyData);
}
