#include <drogon/drogon_test.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <infrastructure/provider/chayns/ChaynsMessageCorrelation.h>
#include <infrastructure/provider/chayns/ChaynsModelCatalog.h>

namespace {

std::filesystem::path fixtureDirectory()
{
    return std::filesystem::path(__FILE__).parent_path() / "fixtures" / "chayns";
}

Json::Value loadFixture(const std::string& name)
{
    const auto path = fixtureDirectory() / name;
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open fixture: " + path.string());
    }
    Json::CharReaderBuilder builder;
    Json::Value value;
    std::string errors;
    if (!Json::parseFromStream(builder, input, &value, &errors)) {
        throw std::runtime_error("invalid fixture " + path.string() + ": " + errors);
    }
    return value;
}

std::string compactJson(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

}  // namespace

DROGON_TEST(ChaynsFixture_AllFilesAreSanitizedEnvelopes)
{
    const std::vector<std::string> names = {
        "thread-create-free.json",
        "thread-create-pro.json",
        "message-create.json",
        "poll-empty.json",
        "poll-messages.json",
        "thread-read.json",
        "thread-delete.json",
        "model-catalog.json",
    };
    const std::vector<std::string> forbidden = {
        "authorization", "cookie", "bearer ", "access_token", "refresh_token",
        "password", "cube.tobit.cloud",
    };
    const std::regex emailPattern(R"([\w.+-]+@[\w.-]+\.[A-Za-z]{2,})");
    const std::regex jwtPattern(
        R"([A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{20,})");

    for (const auto& name : names) {
        const auto fixture = loadFixture(name);
        REQUIRE(fixture.isObject());
        CHECK(fixture["schemaVersion"].asInt() == 1);
        CHECK(fixture["sanitization"].asString() == "explicit-allowlist-v1");
        CHECK(fixture["source"].asString().size() > 4);
        CHECK(fixture["source"].asString().substr(fixture["source"].asString().size() - 4) ==
              ".har");
        REQUIRE(fixture["request"].isObject());
        REQUIRE(fixture["response"].isObject());

        const std::string serialized = compactJson(fixture);
        const std::string lowered = lowerCopy(serialized);
        for (const auto& fragment : forbidden) {
            CHECK(lowered.find(fragment) == std::string::npos);
        }
        CHECK(!std::regex_search(serialized, emailPattern));
        CHECK(!std::regex_search(serialized, jwtPattern));
    }
}

DROGON_TEST(ChaynsFixture_ThreadCreateCapturesFreeAndProWireDifference)
{
    const auto freeFixture = loadFixture("thread-create-free.json");
    const auto proFixture = loadFixture("thread-create-pro.json");
    const auto& freeRequest = freeFixture["request"];
    const auto& proRequest = proFixture["request"];

    CHECK(freeRequest["method"].asString() == "POST");
    CHECK(freeRequest["path"].asString() ==
          "/intercom-backend/v2/thread?forceCreate=true");
    CHECK(freeRequest["body"]["typeId"].asInt() == 8);
    CHECK(!freeRequest["body"].isMember("workspaceUacId"));
    CHECK(proRequest["body"]["typeId"].asInt() == 9);
    CHECK(proRequest["body"]["workspaceUacId"].asInt64() == 9001);
    CHECK(freeFixture["response"]["status"].asInt() == 201);
    CHECK(proFixture["response"]["status"].asInt() == 201);
    CHECK(freeFixture["response"]["body"]["id"].asString() == "<thread-1>");
}

DROGON_TEST(ChaynsFixture_PollBatchCorrelatesReasoningAndFinalAfterAnchor)
{
    const auto fixture = loadFixture("poll-messages.json");
    REQUIRE(fixture["response"]["status"].asInt() == 200);
    const auto& messages = fixture["response"]["body"];
    REQUIRE(messages.isArray());
    REQUIRE(messages.size() == 3);

    chayns::MessageAnchor anchor;
    anchor.messageId = "<request-message>";
    anchor.threadId = "<thread-1>";
    anchor.userAuthorId = "<user-author>";
    anchor.agentAuthorId = "<agent-author>";
    anchor.creationTime = "2026-01-01T00:00:01.000Z";
    std::unordered_set<std::string> consumed;

    const auto result = chayns::correlateMessageBatch(messages, anchor, consumed);
    CHECK(result.status == chayns::CorrelationStatus::FinalFound);
    CHECK(result.reasoningMessages.size() == 1);
    CHECK(result.reasoningMessages[0]["typeId"].asInt() == 18);
    CHECK(result.finalMessage["text"].asString() == "synthetic final answer");
    CHECK(result.finalMessage["id"].asString() == "<message-3>");
}

DROGON_TEST(ChaynsFixture_ModelCatalogContainsUsableFreeAndProModels)
{
    const auto fixture = loadFixture("model-catalog.json");
    REQUIRE(fixture["response"]["status"].asInt() == 200);
    const auto parsed = chayns::parseModelCatalog(fixture["response"]["body"]);
    REQUIRE(parsed.valid);
    REQUIRE(parsed.catalog.byName.size() == 2);

    const auto& freeModel = parsed.catalog.byName.at("fixture-free-model");
    const auto& proModel = parsed.catalog.byName.at("fixture-pro-model");
    CHECK(!freeModel.requiresPro);
    CHECK(proModel.requiresPro);
    CHECK(!freeModel.personId.empty());
    CHECK(!proModel.personId.empty());
    CHECK(chayns::supportsImageInput(freeModel));
}

DROGON_TEST(ChaynsFixture_MessagePollReadDeleteStatusContracts)
{
    const auto message = loadFixture("message-create.json");
    const auto emptyPoll = loadFixture("poll-empty.json");
    const auto read = loadFixture("thread-read.json");
    const auto deletion = loadFixture("thread-delete.json");

    CHECK(message["request"]["method"].asString() == "POST");
    CHECK(message["response"]["status"].asInt() == 201);
    CHECK(emptyPoll["request"]["method"].asString() == "GET");
    CHECK(emptyPoll["response"]["status"].asInt() == 204);
    CHECK(!emptyPoll["response"].isMember("body"));
    CHECK(read["request"]["method"].asString() == "PATCH");
    CHECK(read["response"]["status"].asInt() == 200);
    CHECK(deletion["request"]["method"].asString() == "DELETE");
    CHECK(deletion["response"]["status"].asInt() == 200);
}
