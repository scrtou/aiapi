/**
 * @file test_continuity_resolver.cpp
 * @brief ContinuityResolver 单元测试（最小覆盖）
 */

#include <drogon/drogon_test.h>
#include "sessionManager/continuity/ContinuityResolver.h"
#include "sessionManager/continuity/ResponseIndex.h"
#include "sessionManager/core/Session.h"
#include <tools/ZeroWidthEncoder.h>

namespace {

bool isHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

} // 命名空间结束

DROGON_TEST(ContinuityResolver_Responses_PreviousResponseId_Hit)
{
    auto* mgr = chatSession::getInstance();
    mgr->setTrackingMode(SessionTrackingMode::Hash);

    const std::string sessionId = "sess_test_prev_hit";
    const std::string prevRespId = "resp_test_prev_hit";

    ResponseIndex::instance().erase(prevRespId);
    ResponseIndex::instance().bind(prevRespId, sessionId);

    GenerationRequest req;
    req.endpointType = EndpointType::Responses;
    req.model = "GPT-4o";
    req.previousResponseId = prevRespId;

    ContinuityResolver resolver;
    auto d = resolver.resolve(req);

    CHECK(d.source == ContinuityDecision::Source::PreviousResponseId);
    CHECK(d.sessionId == sessionId);

    ResponseIndex::instance().erase(prevRespId);
}

DROGON_TEST(ContinuityResolver_Responses_PreviousResponseId_Miss_NewSession)
{
    auto* mgr = chatSession::getInstance();
    mgr->setTrackingMode(SessionTrackingMode::Hash);

    const std::string prevRespId = "resp_test_prev_miss";
    ResponseIndex::instance().erase(prevRespId);

    GenerationRequest req;
    req.endpointType = EndpointType::Responses;
    req.model = "GPT-4o";
    req.previousResponseId = prevRespId;

    ContinuityResolver resolver;
    auto d = resolver.resolve(req);

    CHECK(d.source == ContinuityDecision::Source::NewSession);
    CHECK(d.sessionId.rfind("sess_", 0) == 0);
}

DROGON_TEST(ContinuityResolver_ZeroWidth_Decode_Hit)
{
    auto* mgr = chatSession::getInstance();
    mgr->setTrackingMode(SessionTrackingMode::ZeroWidth);

    const std::string sid = "sess_zw_hit_001";
    const std::string raw = ZeroWidthEncoder::appendEncoded("hello", sid);

    GenerationRequest req;
    req.endpointType = EndpointType::ChatCompletions;
    req.model = "GPT-4o";
    req.continuityTexts.push_back(raw);

    ContinuityResolver resolver;
    auto d = resolver.resolve(req);

    CHECK(d.source == ContinuityDecision::Source::ZeroWidth);
    CHECK(d.sessionId == sid);
}

DROGON_TEST(ContinuityResolver_ZeroWidth_Decode_Miss_NewSession)
{
    auto* mgr = chatSession::getInstance();
    mgr->setTrackingMode(SessionTrackingMode::ZeroWidth);

    GenerationRequest req;
    req.endpointType = EndpointType::ChatCompletions;
    req.model = "GPT-4o";
    req.continuityTexts.push_back("no_zerowidth_here");

    ContinuityResolver resolver;
    auto d = resolver.resolve(req);

    CHECK(d.source == ContinuityDecision::Source::NewSession);
    CHECK(d.sessionId.rfind("sess_", 0) == 0);
}

DROGON_TEST(ContinuityResolver_Hash_Path_NonEmpty)
{
    auto* mgr = chatSession::getInstance();
    mgr->setTrackingMode(SessionTrackingMode::Hash);

    GenerationRequest req;
    req.endpointType = EndpointType::ChatCompletions;
    req.model = "GPT-4o";
    req.clientInfo["client_type"] = "test";
    req.messages.push_back(Message::user("hi"));

    ContinuityResolver resolver;
    auto d = resolver.resolve(req);

    CHECK(d.source == ContinuityDecision::Source::Hash);
    CHECK(isHex64(d.sessionId));
}

DROGON_TEST(SessionStore_ContextMap_IsConsumed_By_GetOrCreateSession)
{
    auto* mgr = chatSession::getInstance();
    mgr->setTrackingMode(SessionTrackingMode::Hash);

    // 构造一个基础会话，并走两阶段会话转移（prepare + cover）触发 context_map 生成。
    session_st base;
    base.state.conversationId = "sess_ctx_base_001";
    base.request.model = "GPT-4o";
    base.request.api = "";  // 避免触发 上游 transfer线程Context
    base.provider.clientInfo["client_type"] = "";

    {
        Json::Value m1;
        m1["role"] = "user";
        m1["content"] = "u1";
        base.addMessageToContext(m1);

        Json::Value m2;
        m2["role"] = "assistant";
        m2["content"] = "a1";
        base.addMessageToContext(m2);
    }

    base.request.message = "u2";
    base.request.rawMessage = "u2";
    base.response.message["message"] = "a2";

    mgr->addSession(base.state.conversationId, base);

    // 两阶段会话转移：先预生成 nextSessionId，再提交会话转移。
    mgr->prepareNextSessionId(base);
    mgr->coverSessionresponse(base);

    const std::string realSessionId = base.state.conversationId;
    const std::string trimmedHashKey = base.state.contextConversationId;

    CHECK(isHex64(realSessionId));
    CHECK(isHex64(trimmedHashKey));
    CHECK(!mgr->sessionIsExist(trimmedHashKey));
    CHECK(mgr->sessionIsExist(realSessionId));

    // 新请求命中 trimmedHashKey 时，应映射回 real会话Id，并消费 context_map。
    session_st incoming;
    incoming.request.model = "GPT-4o";
    incoming.request.api = "";
    incoming.provider.clientInfo["client_type"] = "";

    mgr->getOrCreateSession(trimmedHashKey, incoming);

    CHECK(incoming.state.conversationId == realSessionId);
    CHECK(incoming.state.isContinuation);

    // 映射应当已被消费（一次性）
    std::string out;
    CHECK(!mgr->consumeContextMapping(trimmedHashKey, out));

    mgr->delSession(realSessionId);
}
