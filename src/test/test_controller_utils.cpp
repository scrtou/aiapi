#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "../controllers/ControllerUtils.h"

#include <ctime>
#include <future>
#include <memory>
#include <regex>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace
{

// 计数型回调：既捕获响应，也记录被调用次数。
// 只断言「响应内容对」是不够的 —— sendXxx 的契约里「恰好回调一次」同等重要：
// 漏调会让 HTTP 请求永久悬挂，多调会让 drogon 对同一连接二次写入。
struct CallbackSpy
{
    int calls = 0;
    drogon::HttpResponsePtr last;

    std::function<void(const drogon::HttpResponsePtr &)> fn()
    {
        return [this](const drogon::HttpResponsePtr &r) {
            ++calls;
            last = r;
        };
    }
};

Json::Value bodyAsJson(const drogon::HttpResponsePtr &resp)
{
    Json::Value out;
    if (!resp) return out;
    auto j = resp->getJsonObject();
    if (j) out = *j;
    return out;
}

drogon::HttpRequestPtr jsonRequest(const std::string &body)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->setBody(body);
    return req;
}

bool looksLikeUtcStamp(const std::string &s)
{
    static const std::regex re(R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$)");
    return std::regex_match(s, re);
}

// 用 timegm 而非 mktime：被测函数产出的是 UTC 字符串，
// 若用 mktime 反解，差值会被本地时区偏移污染，跨时区跑就红。
std::time_t parseUtcStamp(const std::string &s)
{
    std::tm tm{};
    if (strptime(s.c_str(), "%Y-%m-%d %H:%M:%S", &tm) == nullptr) return -1;
    return timegm(&tm);
}

}  // namespace

// ==================== sendError / makeError ====================

DROGON_TEST(CtlUtilsSendErrorShapesBody)
{
    CallbackSpy spy;
    auto cb = spy.fn();
    ctl::sendError(cb, drogon::k400BadRequest, "invalid_request_error", "missing field: model");

    REQUIRE(spy.calls == 1);
    REQUIRE(spy.last != nullptr);
    CHECK(spy.last->getStatusCode() == drogon::k400BadRequest);

    auto j = bodyAsJson(spy.last);
    CHECK(j["error"]["message"].asString() == "missing field: model");
    CHECK(j["error"]["type"].asString() == "invalid_request_error");
    // 4 参重载不得凭空造出 code 字段。客户端按 code 做分支时,
    // 「code 为空串」与「无 code」是两种不同语义。
    CHECK(j["error"].isMember("code") == false);
}

DROGON_TEST(CtlUtilsSendErrorWithCodeAddsField)
{
    CallbackSpy spy;
    auto cb = spy.fn();
    ctl::sendError(cb, drogon::k429TooManyRequests, "rate_limit_error", "slow down", "rate_limited");

    REQUIRE(spy.calls == 1);
    CHECK(spy.last->getStatusCode() == drogon::k429TooManyRequests);
    auto j = bodyAsJson(spy.last);
    CHECK(j["error"]["code"].asString() == "rate_limited");
    CHECK(j["error"]["type"].asString() == "rate_limit_error");
}

DROGON_TEST(CtlUtilsMakeErrorOmitsEmptyCode)
{
    // makeError 只造响应、不回调 —— 与 sendError 的区别正是 respondInLoop 场景所需。
    auto resp = ctl::makeError(drogon::k404NotFound, "not_found", "no such account");
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == drogon::k404NotFound);
    auto j = bodyAsJson(resp);
    CHECK(j["error"]["message"].asString() == "no such account");
    CHECK(j["error"].isMember("code") == false);
}

DROGON_TEST(CtlUtilsMakeErrorKeepsNonEmptyCode)
{
    auto resp = ctl::makeError(drogon::k500InternalServerError, "server_error", "boom", "db_down");
    REQUIRE(resp != nullptr);
    auto j = bodyAsJson(resp);
    CHECK(j["error"]["code"].asString() == "db_down");
}

// ==================== sendJson / sendJsonUtf8 ====================

DROGON_TEST(CtlUtilsSendJsonDefaultsTo200)
{
    CallbackSpy spy;
    auto cb = spy.fn();
    Json::Value data;
    data["ok"] = true;
    ctl::sendJson(cb, data);

    REQUIRE(spy.calls == 1);
    CHECK(spy.last->getStatusCode() == drogon::k200OK);
    CHECK(bodyAsJson(spy.last)["ok"].asBool() == true);
}

DROGON_TEST(CtlUtilsSendJsonHonoursExplicitStatus)
{
    CallbackSpy spy;
    auto cb = spy.fn();
    Json::Value data;
    data["id"] = 7;
    ctl::sendJson(cb, data, drogon::k201Created);

    REQUIRE(spy.calls == 1);
    CHECK(spy.last->getStatusCode() == drogon::k201Created);
}

DROGON_TEST(CtlUtilsSendJsonUtf8SetsCharset)
{
    // 注意：截至本次提交，sendJsonUtf8 在 src/ 下有 0 处生产调用（实测计数）。
    // 测它不是为了守某条现有路径,而是因为它就摆在共享 header 里随时会被用起来；
    // 若哪天有人用了它而 charset 丢了,中文响应会在部分客户端上乱码。
    CallbackSpy spy;
    auto cb = spy.fn();
    Json::Value data;
    data["msg"] = "中文";
    ctl::sendJsonUtf8(cb, data);

    REQUIRE(spy.calls == 1);
    const std::string ct = spy.last->contentTypeString();
    CHECK(ct.find("application/json") != std::string::npos);
    CHECK(ct.find("charset=utf-8") != std::string::npos);
}

// ==================== parseJsonOrError ====================

DROGON_TEST(CtlUtilsParseJsonAcceptsValidBodyWithoutResponding)
{
    CallbackSpy spy;
    auto cb = spy.fn();
    std::shared_ptr<Json::Value> out;
    const bool ok = ctl::parseJsonOrError(jsonRequest(R"({"model":"gpt-4"})"), cb, out);

    CHECK(ok == true);
    REQUIRE(out != nullptr);
    CHECK((*out)["model"].asString() == "gpt-4");
    // 成功路径上绝不能回调：调用方紧接着还要用同一个 callback 发真正的响应,
    // 这里若误回调一次,生产上就是「同一请求被响应两次」。
    CHECK(spy.calls == 0);
}

DROGON_TEST(CtlUtilsParseJsonRejectsGarbageWithDefaults)
{
    CallbackSpy spy;
    auto cb = spy.fn();
    std::shared_ptr<Json::Value> out;
    const bool ok = ctl::parseJsonOrError(jsonRequest("{not json at all"), cb, out);

    CHECK(ok == false);
    REQUIRE(spy.calls == 1);
    CHECK(spy.last->getStatusCode() == drogon::k400BadRequest);
    auto j = bodyAsJson(spy.last);
    CHECK(j["error"]["type"].asString() == "invalid_request_error");
    CHECK(j["error"]["message"].asString() == "Invalid JSON in request body");
}

DROGON_TEST(CtlUtilsParseJsonHonoursCustomErrorParams)
{
    // AiApiController.cc:313 正是用自定义 type 调它的,这条用例守的是那处行为。
    CallbackSpy spy;
    auto cb = spy.fn();
    std::shared_ptr<Json::Value> out;
    const bool ok = ctl::parseJsonOrError(
        jsonRequest("<<<"), cb, out, "invalid_json", "body must be JSON", drogon::k422UnprocessableEntity);

    CHECK(ok == false);
    REQUIRE(spy.calls == 1);
    CHECK(spy.last->getStatusCode() == drogon::k422UnprocessableEntity);
    auto j = bodyAsJson(spy.last);
    CHECK(j["error"]["type"].asString() == "invalid_json");
    CHECK(j["error"]["message"].asString() == "body must be JSON");
}

// ==================== defaultTimeRange ====================

DROGON_TEST(CtlUtilsDefaultTimeRangeLeavesBothWhenSet)
{
    std::string from = "2020-01-01 00:00:00";
    std::string to = "2020-01-02 00:00:00";
    ctl::defaultTimeRange(from, to);
    CHECK(from == "2020-01-01 00:00:00");
    CHECK(to == "2020-01-02 00:00:00");
}

DROGON_TEST(CtlUtilsDefaultTimeRangeFillsBothWhenEmpty)
{
    std::string from;
    std::string to;
    ctl::defaultTimeRange(from, to);

    REQUIRE(looksLikeUtcStamp(from));
    REQUIRE(looksLikeUtcStamp(to));
    const auto a = parseUtcStamp(from);
    const auto b = parseUtcStamp(to);
    REQUIRE(a > 0);
    REQUIRE(b > 0);
    const auto span = static_cast<long long>(b - a);
    // 容差 2 秒：now 取一次但格式化两次,秒级截断可能差 1。
    CHECK(span >= 24 * 3600 - 2);
    CHECK(span <= 24 * 3600 + 2);
}

DROGON_TEST(CtlUtilsDefaultTimeRangeHonoursHoursArg)
{
    std::string from;
    std::string to;
    ctl::defaultTimeRange(from, to, 1);

    const auto span = static_cast<long long>(parseUtcStamp(to) - parseUtcStamp(from));
    CHECK(span >= 3600 - 2);
    CHECK(span <= 3600 + 2);
}

DROGON_TEST(CtlUtilsDefaultTimeRangeFillsOnlyMissingTo)
{
    std::string from = "2020-01-01 00:00:00";
    std::string to;
    ctl::defaultTimeRange(from, to);

    CHECK(from == "2020-01-01 00:00:00");
    CHECK(looksLikeUtcStamp(to));
}

DROGON_TEST(CtlUtilsDefaultTimeRangeFillsFromRelativeToNowNotTo)
{
    // 这条用例固化的是一个反直觉但确实存在的行为：
    // 只给 to 时,from 补的是「现在往前 hours」,而不是「to 往前 hours」。
    // 于是传一个历史 to 会得到 from > to 的倒挂区间。
    // 先把现状钉住；是否算缺陷需要产品侧判断,不在本次改动范围内擅自改语义。
    std::string from;
    std::string to = "2020-01-02 03:04:05";
    ctl::defaultTimeRange(from, to);

    CHECK(to == "2020-01-02 03:04:05");
    REQUIRE(looksLikeUtcStamp(from));
    CHECK(parseUtcStamp(from) > parseUtcStamp(to));
}

// ==================== respondInLoop ====================

DROGON_TEST(CtlUtilsRespondInLoopDispatchesOnLoopThread)
{
    auto resp = ctl::makeError(drogon::k503ServiceUnavailable, "unavailable", "try later");

    std::promise<std::thread::id> p;
    auto f = p.get_future();
    auto seen = std::make_shared<drogon::HttpResponsePtr>();

    auto holder = std::make_shared<std::function<void(const drogon::HttpResponsePtr &)>>(
        [&p, seen](const drogon::HttpResponsePtr &r) {
            *seen = r;                              // 先落地再 set_value,保证 wait 返回后 seen 已可读
            p.set_value(std::this_thread::get_id());
        });

    ctl::respondInLoop(holder, resp);

    REQUIRE(f.wait_for(2s) == std::future_status::ready);
    // 关键断言：回调必须发生在 loop 线程,而不是当前测试线程。
    // 若哪天有人把 queueInLoop 改成直调,这条会红 —— 那种改动会让
    // 非 loop 线程直接写连接,是真实的数据竞争。
    CHECK(f.get() != std::this_thread::get_id());
    REQUIRE(*seen != nullptr);
    CHECK((*seen)->getStatusCode() == drogon::k503ServiceUnavailable);
}

DROGON_TEST(CtlUtilsRespondInLoopIgnoresNullAndEmptyCallback)
{
    auto resp = ctl::makeError(drogon::k500InternalServerError, "server_error", "boom");

    std::shared_ptr<std::function<void(const drogon::HttpResponsePtr &)>> nullHolder;
    ctl::respondInLoop(nullHolder, resp);                 // 空 shared_ptr

    auto emptyHolder = std::make_shared<std::function<void(const drogon::HttpResponsePtr &)>>();
    ctl::respondInLoop(emptyHolder, resp);                // 非空 shared_ptr 但 function 为空

    // 「什么都没发生」无法直接断言,改用一次排空来证明 loop 既没被投入垃圾任务、也没被搞坏。
    std::promise<void> drained;
    auto f = drained.get_future();
    drogon::app().getLoop()->queueInLoop([&drained]() { drained.set_value(); });
    REQUIRE(f.wait_for(2s) == std::future_status::ready);
}
