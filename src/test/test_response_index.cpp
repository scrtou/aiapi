/**
 * @file test_response_index.cpp
 * @brief ResponseIndex 单元测试
 */

#include <drogon/drogon_test.h>
#include <application/generation/continuity/ResponseIndex.h>
#include <thread>
#include <sstream>

namespace {
class MemoryResponsePersistence final : public ISessionPersistence
{
  public:
    std::optional<ResponsePersistenceRow> response;
    std::vector<std::string> deleted;
    bool ensureTables(std::string*) override { return true; }
    bool isEnabled() const override { return true; }
    void setEnabled(bool) override {}
    std::optional<SessionPersistenceRow> loadSession(const std::string&, std::string*) override
    { return std::nullopt; }
    std::optional<SessionPersistenceRow> loadSessionByContextKey(const std::string&, std::string*) override
    { return std::nullopt; }
    std::optional<ResponsePersistenceRow> loadResponse(const std::string& id, std::string*) override
    { return response && response->responseId == id ? response : std::nullopt; }
    void asyncUpsertSession(const SessionPersistenceRow&) override {}
    void asyncUpsertResponse(const ResponsePersistenceRow& row) override { response = row; }
    void asyncDeleteSessions(const std::vector<std::string>&) override {}
    void asyncDeleteResponses(const std::vector<std::string>& ids) override { deleted = ids; }
    int deleteSessionsOlderThan(int64_t, std::string*) override { return 0; }
};
}

DROGON_TEST(ResponseIndex_BindAndGet)
{
    ResponseIndex index;
    const std::string respId = "resp_test_bind_get";
    const std::string sessId = "sess_test_bind_get";

    index.erase(respId);

    index.bind(respId, sessId);

    std::string out;
    CHECK(index.tryGetSessionId(respId, out));
    CHECK(out == sessId);

    index.erase(respId);
}

DROGON_TEST(ResponseIndex_StoreAndGetResponse)
{
    ResponseIndex index;
    const std::string respId = "resp_test_store_get";
    index.erase(respId);

    Json::Value resp;
    resp["id"] = respId;
    resp["object"] = "response";
    resp["status"] = "completed";

    index.storeResponse(respId, resp.toStyledString());

    std::string outJson;
    Json::Value out;
    REQUIRE(index.tryGetResponse(respId, outJson));
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(outJson);
    REQUIRE(Json::parseFromStream(builder, stream, &out, &errors));
    CHECK(out["id"].asString() == respId);
    CHECK(out["status"].asString() == "completed");

    index.erase(respId);
}

DROGON_TEST(ResponseIndex_Cleanup_MaxEntries)
{
    ResponseIndex index;
    const std::string r1 = "resp_test_cleanup_entries_1";
    const std::string r2 = "resp_test_cleanup_entries_2";
    const std::string r3 = "resp_test_cleanup_entries_3";

    index.erase(r1);
    index.erase(r2);
    index.erase(r3);

    index.bind(r1, "sess1");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    index.bind(r2, "sess2");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    index.bind(r3, "sess3");

    index.cleanup(2, std::chrono::seconds(0));

    std::string out;
    bool h1 = index.tryGetSessionId(r1, out);
    bool h2 = index.tryGetSessionId(r2, out);
    bool h3 = index.tryGetSessionId(r3, out);

    CHECK((h1 ? 1 : 0) + (h2 ? 1 : 0) + (h3 ? 1 : 0) == 2);

    index.erase(r1);
    index.erase(r2);
    index.erase(r3);
}

DROGON_TEST(ResponseIndex_Cleanup_MaxAge)
{
    ResponseIndex index;
    const std::string respId = "resp_test_cleanup_age";
    index.erase(respId);

    index.bind(respId, "sess_age");

    // maxAge 是 级别；这里等待 > 1s 以触发清理
    std::this_thread::sleep_for(std::chrono::seconds(2));
    index.cleanup(ResponseIndex::kDefaultMaxEntries, std::chrono::seconds(1));

    std::string out;
    CHECK(!index.tryGetSessionId(respId, out));

    index.erase(respId);
}

DROGON_TEST(ResponseIndexPersistenceUsesInjectedPort)
{
    MemoryResponsePersistence store;
    ResponseIndex writer(&store);
    writer.setPersistenceEnabled(true);
    writer.bind("resp_injected", "session_injected");
    REQUIRE(store.response.has_value());
    CHECK(store.response->sessionId == "session_injected");

    ResponseIndex reader(&store);
    reader.setPersistenceEnabled(true);
    std::string sessionId;
    CHECK(reader.tryGetSessionId("resp_injected", sessionId));
    CHECK(sessionId == "session_injected");

    CHECK(reader.erase("resp_injected"));
    REQUIRE(store.deleted.size() == 1);
    CHECK(store.deleted[0] == "resp_injected");
}
