/**
 * @file test_continuity_resolver.cpp
 * @brief ContinuityResolver 单元测试（最小覆盖）
 */

#include <drogon/drogon_test.h>

#include "../sessionManager/ContinuityResolver.h"
#include "../sessionManager/ResponseIndex.h"
#include "../sessionManager/Session.h"

#include <tools/ZeroWidthEncoder.h>

namespace {

bool isHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

} // namespace

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

    // 构造一个基础会话，并通过 coverSessionresponse 触发 context_map 生成（保持旧逻辑）。
    session_st base;
    base.curConversationId = "sess_ctx_base_001";
    base.selectmodel = "GPT-4o";
    base.selectapi = "";  // 避免触发 provider transferThreadContext
    base.client_info["client_type"] = "";

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

    base.requestmessage = "u2";
    base.requestmessage_raw = "u2";
    base.responsemessage["message"] = "a2";

    mgr->addSession(base.curConversationId, base);

    // 该调用会：
    // - 追加 u2/a2 到 message_context
    // - 重算 newConversationId 并 re-key session_map
    // - 生成 context_map（用于“裁剪 hashKey”映射回真实 sessionId）
    mgr->coverSessionresponse(base);

    const std::string realSessionId = base.curConversationId;
    const std::string trimmedHashKey = base.contextConversationId;

    CHECK(isHex64(realSessionId));
    CHECK(isHex64(trimmedHashKey));
    CHECK(!mgr->sessionIsExist(trimmedHashKey));
    CHECK(mgr->sessionIsExist(realSessionId));

    // 新请求命中 trimmedHashKey 时，应映射回 realSessionId，并消费 context_map。
    session_st incoming;
    incoming.selectmodel = "GPT-4o";
    incoming.selectapi = "";
    incoming.client_info["client_type"] = "";

    mgr->getOrCreateSession(trimmedHashKey, incoming);

    CHECK(incoming.curConversationId == realSessionId);
    CHECK(incoming.is_continuation);

    // 映射应当已被消费（一次性）
    std::string out;
    CHECK(!mgr->consumeContextMapping(trimmedHashKey, out));

    mgr->delSession(realSessionId);
}
