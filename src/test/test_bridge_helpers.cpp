#include <drogon/drogon_test.h>

#include <sessionManager/tooling/BridgeHelpers.h>

#include <string>

// ---------------------------------------------------------------------------
// RFC-001 §0-E-1 / R2: BridgeHelpers.h fanin=6, 已链接进测试二进制, 但零断言。
// 本文件只覆盖 **纯函数**; recordErrorStat / recordWarnStat /
// recordRequestCompletedStat 依赖 ErrorStatsService 单例 + 后台线程,
// 属于副作用函数, 需要独立的 fixture, 不在本轮范围内。
// 所有断言均依据 BridgeHelpers.cpp 的实际实现, 而非期望行为。
// ---------------------------------------------------------------------------

namespace {

session_st makeSession(const std::string& trigger = "") {
    session_st s;
    s.provider.toolBridgeTrigger = trigger;
    return s;
}

}  // namespace

// ========================= trimWhitespace =========================

DROGON_TEST(BridgeHelpersTrimWhitespace) {
    CHECK(bridge::trimWhitespace("  hello  ") == "hello");
    CHECK(bridge::trimWhitespace("\t\n hi \r\n") == "hi");
    CHECK(bridge::trimWhitespace("nospace") == "nospace");
    // 全空白 -> 空串 (find_first_not_of == npos 分支)
    CHECK(bridge::trimWhitespace("   \t\r\n ") == "");
    CHECK(bridge::trimWhitespace("") == "");
    // 内部空白必须保留
    CHECK(bridge::trimWhitespace("  a b  ") == "a b");
}

// ========================= stripMarkdownCodeFence =========================

DROGON_TEST(BridgeHelpersStripMarkdownCodeFence) {
    // 带语言标签的围栏
    CHECK(bridge::stripMarkdownCodeFence("```json\n{\"a\":1}\n```") == "{\"a\":1}");
    // 不带语言标签
    CHECK(bridge::stripMarkdownCodeFence("```\nplain\n```") == "plain");
    // 围栏外有空白, 会先被 trim
    CHECK(bridge::stripMarkdownCodeFence("  ```\nx\n```  ") == "x");
    // 无围栏 -> 原样返回(已 trim)
    CHECK(bridge::stripMarkdownCodeFence("  no fence  ") == "no fence");
    // 只有开头围栏、没有换行 -> 原样返回
    CHECK(bridge::stripMarkdownCodeFence("```abc") == "```abc");
    // 围栏内多行内容保留内部换行
    CHECK(bridge::stripMarkdownCodeFence("```\nl1\nl2\n```") == "l1\nl2");
}

// ========================= toLowerStr =========================

DROGON_TEST(BridgeHelpersToLowerStr) {
    CHECK(bridge::toLowerStr("ABC") == "abc");
    CHECK(bridge::toLowerStr("MiXeD123") == "mixed123");
    CHECK(bridge::toLowerStr("") == "");
    // 非字母字节不受影响
    CHECK(bridge::toLowerStr("A-B_C") == "a-b_c");
}

// ========================= safeJsonAsString =========================

DROGON_TEST(BridgeHelpersSafeJsonAsString) {
    CHECK(bridge::safeJsonAsString(Json::Value("txt")) == "txt");
    // null -> 默认值
    CHECK(bridge::safeJsonAsString(Json::Value(), "fallback") == "fallback");
    CHECK(bridge::safeJsonAsString(Json::Value()) == "");
    // 非字符串标量被序列化, 而不是走默认值
    CHECK(bridge::safeJsonAsString(Json::Value(42)) == "42");
    CHECK(bridge::safeJsonAsString(Json::Value(true)) == "true");
    // 对象被紧凑序列化(indentation 置空)
    Json::Value obj(Json::objectValue);
    obj["k"] = "v";
    const std::string dumped = bridge::safeJsonAsString(obj);
    CHECK(dumped.find("\"k\"") != std::string::npos);
    CHECK(dumped.find('\n') == std::string::npos);
}

// ========================= ltrimView / startsWithStr =========================

DROGON_TEST(BridgeHelpersLtrimViewAndStartsWith) {
    const std::string padded = "   abc";
    CHECK(bridge::ltrimView(padded) == "abc");
    const std::string blank = "   ";
    CHECK(bridge::ltrimView(blank).empty());
    const std::string clean = "abc";
    CHECK(bridge::ltrimView(clean) == "abc");

    CHECK(bridge::startsWithStr("hello", "he"));
    CHECK(!bridge::startsWithStr("hello", "lo"));
    // 前缀长于本体 -> false, 不得越界
    CHECK(!bridge::startsWithStr("ab", "abc"));
    // 空前缀恒真
    CHECK(bridge::startsWithStr("", ""));
    CHECK(bridge::startsWithStr("x", ""));
}

// ========================= normalizeBridgeXml =========================

DROGON_TEST(BridgeHelpersNormalizeBridgeXml) {
    // CRLF / CR 统一为 LF
    CHECK(bridge::normalizeBridgeXml("a\r\nb") == "a\nb");
    CHECK(bridge::normalizeBridgeXml("a\rb") == "a\nb");
    // NBSP (U+00A0) -> 普通空格
    CHECK(bridge::normalizeBridgeXml("a\xC2\xA0" "b") == "a b");
    // 全角空格 (U+3000) -> 普通空格
    CHECK(bridge::normalizeBridgeXml("a\xE3\x80\x80" "b") == "a b");
    // 普通文本不被改动
    CHECK(bridge::normalizeBridgeXml("<tag>ok</tag>") == "<tag>ok</tag>");
    CHECK(bridge::normalizeBridgeXml("") == "");
}

// ========================= extractXmlInputForToolCalls =========================

DROGON_TEST(BridgeHelpersExtractXmlNoTrigger) {
    // trigger 为空 -> 无条件返回空串(即使正文里有 function_calls)
    const session_st s = makeSession("");
    CHECK(bridge::extractXmlInputForToolCalls(s, "<function_calls>x", true).empty());
    CHECK(bridge::extractXmlInputForToolCalls(s, "anything").empty());
}

DROGON_TEST(BridgeHelpersExtractXmlByTrigger) {
    const session_st s = makeSession("<Function_ABCD_Start/>");
    // 命中 trigger -> 从 trigger 起始位置截断, 丢弃前置散文
    const std::string raw = "prefix text <Function_ABCD_Start/>{\"a\":1}";
    CHECK(bridge::extractXmlInputForToolCalls(s, raw) ==
          "<Function_ABCD_Start/>{\"a\":1}");
    // trigger 未出现 且 未开启 fallback -> 空串
    CHECK(bridge::extractXmlInputForToolCalls(s, "no trigger here").empty());
    // markdown 围栏先被剥离, 再找 trigger
    CHECK(bridge::extractXmlInputForToolCalls(
              s, "```\n<Function_ABCD_Start/>payload\n```") ==
          "<Function_ABCD_Start/>payload");
}

DROGON_TEST(BridgeHelpersExtractXmlFallback) {
    const session_st s = makeSession("<Function_ABCD_Start/>");
    const std::string raw = "blah <function_calls>body</function_calls>";
    // fallback 关闭 -> 空串
    CHECK(bridge::extractXmlInputForToolCalls(s, raw, false).empty());
    // fallback 打开 -> 从 <function_calls 起截断
    CHECK(bridge::extractXmlInputForToolCalls(s, raw, true) ==
          "<function_calls>body</function_calls>");
    // fallback 打开但两者都没有 -> 仍是空串
    CHECK(bridge::extractXmlInputForToolCalls(s, "plain prose", true).empty());
}

// ========================= 随机 ID / 触发信号 =========================

DROGON_TEST(BridgeHelpersGenerateFallbackToolCallId) {
    const std::string id = bridge::generateFallbackToolCallId();
    // "call_" + 12 字节 * 2 位十六进制
    CHECK(bridge::startsWithStr(id, "call_"));
    CHECK(id.size() == 5 + 24);
    CHECK(id.find_first_not_of("0123456789abcdef", 5) == std::string::npos);
    // 两次调用不应相同(碰撞概率 2^-96)
    CHECK(id != bridge::generateFallbackToolCallId());
}

DROGON_TEST(BridgeHelpersGenerateRandomTriggerSignal) {
    const std::string sig = bridge::generateRandomTriggerSignal(8);
    CHECK(bridge::startsWithStr(sig, "<Function_"));
    // "<Function_"(10) + 8 + "_Start/>"(8)
    CHECK(sig.size() == 26);
    CHECK(sig.substr(sig.size() - 8) == "_Start/>");

    // 下界钳制: <4 一律按 4 处理
    CHECK(bridge::generateRandomTriggerSignal(0).size() == 18 + 4);
    CHECK(bridge::generateRandomTriggerSignal(1).size() == 18 + 4);
    // 上界钳制: >32 一律按 32 处理
    CHECK(bridge::generateRandomTriggerSignal(100).size() == 18 + 32);
    // 随机段只含字母数字
    const std::string body = sig.substr(10, sig.size() - 18);
    CHECK(body.find_first_not_of(
              "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ==
          std::string::npos);
}

// ========================= session 派生字段 =========================

DROGON_TEST(BridgeHelpersClientTypeFromSession) {
    session_st s;
    // clientInfo 未设置 -> 空串, 不得抛异常
    CHECK(bridge::getClientTypeFromSession(s) == "");

    s.provider.clientInfo["client_type"] = "Codex";
    CHECK(bridge::getClientTypeFromSession(s) == "Codex");

    // 非字符串取值会被序列化而非当成缺失
    session_st s2;
    s2.provider.clientInfo["client_type"] = 7;
    CHECK(bridge::getClientTypeFromSession(s2) == "7");
}

DROGON_TEST(BridgeHelpersApiKindFromSession) {
    session_st chat;
    // 默认 apiType 为 ChatCompletions
    CHECK(bridge::getApiKindFromSession(chat) == "chat_completions");

    session_st resp;
    resp.state.apiType = ApiType::Responses;
    CHECK(bridge::getApiKindFromSession(resp) == "responses");
}
