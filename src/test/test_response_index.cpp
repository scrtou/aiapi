/**
 * @file test_response_index.cpp
 * @brief ResponseIndex 单元测试
 */

#include <drogon/drogon_test.h>
#include "sessionManager/continuity/ResponseIndex.h"
#include <thread>

DROGON_TEST(ResponseIndex_BindAndGet)
{
    const std::string respId = "resp_test_bind_get";
    const std::string sessId = "sess_test_bind_get";

    ResponseIndex::instance().erase(respId);

    ResponseIndex::instance().bind(respId, sessId);

    std::string out;
    CHECK(ResponseIndex::instance().tryGetSessionId(respId, out));
    CHECK(out == sessId);

    ResponseIndex::instance().erase(respId);
}

DROGON_TEST(ResponseIndex_StoreAndGetResponse)
{
    const std::string respId = "resp_test_store_get";
    ResponseIndex::instance().erase(respId);

    Json::Value resp;
    resp["id"] = respId;
    resp["object"] = "response";
    resp["status"] = "completed";

    ResponseIndex::instance().storeResponse(respId, resp);

    Json::Value out;
    CHECK(ResponseIndex::instance().tryGetResponse(respId, out));
    CHECK(out["id"].asString() == respId);
    CHECK(out["status"].asString() == "completed");

    ResponseIndex::instance().erase(respId);
}

DROGON_TEST(ResponseIndex_Cleanup_MaxEntries)
{
    const std::string r1 = "resp_test_cleanup_entries_1";
    const std::string r2 = "resp_test_cleanup_entries_2";
    const std::string r3 = "resp_test_cleanup_entries_3";

    ResponseIndex::instance().erase(r1);
    ResponseIndex::instance().erase(r2);
    ResponseIndex::instance().erase(r3);

    ResponseIndex::instance().bind(r1, "sess1");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ResponseIndex::instance().bind(r2, "sess2");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ResponseIndex::instance().bind(r3, "sess3");

    ResponseIndex::instance().cleanup(2, std::chrono::seconds(0));

    std::string out;
    bool h1 = ResponseIndex::instance().tryGetSessionId(r1, out);
    bool h2 = ResponseIndex::instance().tryGetSessionId(r2, out);
    bool h3 = ResponseIndex::instance().tryGetSessionId(r3, out);

    CHECK((h1 ? 1 : 0) + (h2 ? 1 : 0) + (h3 ? 1 : 0) == 2);

    ResponseIndex::instance().erase(r1);
    ResponseIndex::instance().erase(r2);
    ResponseIndex::instance().erase(r3);
}

DROGON_TEST(ResponseIndex_Cleanup_MaxAge)
{
    const std::string respId = "resp_test_cleanup_age";
    ResponseIndex::instance().erase(respId);

    ResponseIndex::instance().bind(respId, "sess_age");

    // maxAge 是 级别；这里等待 > 1s 以触发清理
    std::this_thread::sleep_for(std::chrono::seconds(2));
    ResponseIndex::instance().cleanup(ResponseIndex::kDefaultMaxEntries, std::chrono::seconds(1));

    std::string out;
    CHECK(!ResponseIndex::instance().tryGetSessionId(respId, out));

    ResponseIndex::instance().erase(respId);
}

