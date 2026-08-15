#include <drogon/drogon_test.h>

#include <application/account/accountManager.h>
#include <infrastructure/provider/chayns/ChaynsHttpTransport.h>
#include <infrastructure/provider/chayns/ChaynsClock.h>
#include <infrastructure/provider/chayns/chaynsapi.h>
#include <domain/port/IAccountStore.h>
#include <infrastructure/provider/ProviderRegistry.h>
#include <application/generation/continuity/ResponseIndex.h>
#include <application/generation/core/SessionExecutionGate.h>
#include <application/generation/core/Session.h>
#include <transport/controllers/sinks/ChatJsonSink.h>
#include <transport/controllers/sinks/ChatSseSink.h>
#include <transport/controllers/sinks/ResponsesJsonSink.h>
#include <transport/controllers/sinks/ResponsesSseSink.h>
#include <application/generation/core/GenerationService.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

Json::Value loadChaynsFixture(const std::string& name)
{
    const auto path = std::filesystem::path(__FILE__).parent_path() /
                      "fixtures" / "chayns" / name;
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open fixture: " + path.string());
    }
    Json::CharReaderBuilder builder;
    Json::Value value;
    std::string errors;
    if (!Json::parseFromStream(builder, input, &value, &errors)) {
        throw std::runtime_error("invalid fixture: " + errors);
    }
    return value;
}

drogon::HttpMethod methodFromString(const std::string& method)
{
    if (method == "GET") return drogon::HttpMethod::Get;
    if (method == "POST") return drogon::HttpMethod::Post;
    if (method == "PATCH") return drogon::HttpMethod::Patch;
    if (method == "DELETE") return drogon::HttpMethod::Delete;
    throw std::runtime_error("unsupported fixture method: " + method);
}

struct ExpectedExchange
{
    drogon::HttpMethod method;
    std::string path;
    Json::Value request;
    Json::Value response;
};

class FixtureChaynsTransport final : public chayns::IChaynsHttpTransport
{
  public:
    void enqueue(const std::string& fixtureName)
    {
        const auto fixture = loadChaynsFixture(fixtureName);
        std::string path = fixture["request"]["path"].asString();
        const auto query = path.find('?');
        if (query != std::string::npos && path.find("forceCreate=true") == std::string::npos) {
            path.erase(query);
        }
        // The recorded path carries a template; the production request carries
        // the synthetic thread ID returned by the create response.
        const std::string marker = "{threadId}";
        const auto markerAt = path.find(marker);
        if (markerAt != std::string::npos) {
            path.replace(markerAt, marker.size(), "<thread-1>");
        }
        exchanges_.push_back(
            {methodFromString(fixture["request"]["method"].asString()),
             path,
             fixture["request"],
             fixture["response"]});
    }

    chayns::HttpResult send(const std::string& baseUrl,
                            const drogon::HttpRequestPtr& request,
                            double timeoutSeconds) override
    {
        calledBaseUrls.push_back(baseUrl);
        calledPaths.push_back(request ? request->getPath() : "<null>");
        if (!request) {
            errors.push_back("null request");
            return {drogon::ReqResult::BadResponse, nullptr};
        }
        if (timeoutSeconds <= 0.0) errors.push_back("non-positive timeout");
        if (exchanges_.empty()) {
            errors.push_back("unexpected request: " + request->getPath());
            return {drogon::ReqResult::BadResponse, nullptr};
        }

        const auto exchange = exchanges_.front();
        exchanges_.pop_front();
        if (request->method() != exchange.method) {
            errors.push_back("method mismatch for " + request->getPath());
        }
        if (request->getPath() != exchange.path) {
            errors.push_back("path mismatch: " + request->getPath() + " != " + exchange.path);
        }
        checkRequestBody(exchange, request);

        drogon::HttpResponsePtr response;
        if (exchange.response.isMember("body")) {
            response = drogon::HttpResponse::newHttpJsonResponse(exchange.response["body"]);
        } else {
            response = drogon::HttpResponse::newHttpResponse();
        }
        response->setStatusCode(
            static_cast<drogon::HttpStatusCode>(exchange.response["status"].asInt()));
        if (afterSend) {
            afterSend(request);
        }
        return {drogon::ReqResult::Ok, response};
    }

    std::size_t remaining() const { return exchanges_.size(); }

    std::vector<std::string> errors;
    std::vector<std::string> calledBaseUrls;
    std::vector<std::string> calledPaths;
    std::function<void(const drogon::HttpRequestPtr&)> afterSend;

  private:
    void checkRequestBody(const ExpectedExchange& exchange,
                          const drogon::HttpRequestPtr& request)
    {
        if (!exchange.request.isMember("body")) return;
        const auto actual = request->getJsonObject();
        if (!actual || !actual->isObject()) {
            errors.push_back("missing JSON request body for " + request->getPath());
            return;
        }
        const auto& expected = exchange.request["body"];
        for (const char* key : {"typeId", "workspaceUacId", "isRead", "personId"}) {
            if (expected.isMember(key) && (*actual)[key] != expected[key]) {
                errors.push_back(std::string("request field mismatch: ") + key);
            }
            if (!expected.isMember(key) && actual->isMember(key) &&
                std::string(key) == "workspaceUacId") {
                errors.push_back("free request unexpectedly contains workspaceUacId");
            }
        }
        if (expected.isMember("members")) {
            if (!actual->isMember("members") || (*actual)["members"].size() != 2) {
                errors.push_back("thread create members mismatch");
            } else if ((*actual)["members"][0]["personId"].asString() != "<user-person>" ||
                       (*actual)["members"][1]["personId"].asString().find("<model-person-") != 0) {
                errors.push_back("thread create synthetic member IDs mismatch");
            }
        }
        if (expected.isMember("messages")) {
            if (!actual->isMember("messages") || (*actual)["messages"].empty() ||
                !(*actual)["messages"][0]["text"].isString() ||
                (*actual)["messages"][0]["text"].asString().empty()) {
                errors.push_back("thread create message missing");
            }
        }
        if (expected.isMember("text") && (*actual)["text"] != expected["text"]) {
            errors.push_back("follow-up text mismatch");
        }
        if (expected.isMember("cursorPosition") &&
            (*actual)["cursorPosition"] != expected["cursorPosition"]) {
            errors.push_back("follow-up cursorPosition mismatch");
        }
    }

    std::deque<ExpectedExchange> exchanges_;
};

class ChaynsFixtureAccountStore final : public IAccountStore
{
  public:
    std::list<Accountinfo_st> rows;

    bool addAccount(Accountinfo_st) override { return true; }
    bool updateAccount(Accountinfo_st) override { return true; }
    bool deleteAccount(std::string, std::string) override { return true; }
    bool isTableExist() override { return true; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    std::list<Accountinfo_st> getAccountDBList() override { return rows; }
    int createWaitingAccount(std::string) override { return 0; }
    bool activateAccount(int, Accountinfo_st) override { return true; }
    bool deleteWaitingAccount(int) override { return true; }
    int countAccountsByChannel(std::string, bool) override { return 0; }
    bool updateAccountStatusById(int, std::string) override { return true; }
    std::string getAccountStatusByUsername(std::string, std::string) override
    {
        return AccountStatus::ACTIVE;
    }
};

class DeadlineJumpClock final : public chayns::IChaynsClock
{
  public:
    Clock::time_point now() const override { return now_; }

    void sleepFor(std::chrono::milliseconds duration) override
    {
        ++sleepCalls;
        requestedSleeps.push_back(duration);
        now_ += std::max(duration, std::chrono::duration_cast<std::chrono::milliseconds>(
                                      chayns::kRequestPollingDeadline));
    }

    int sleepCalls = 0;
    std::vector<std::chrono::milliseconds> requestedSleeps;

  private:
    Clock::time_point now_{std::chrono::hours(1)};
};

Accountinfo_st chaynsAccount(const std::string& type)
{
    Accountinfo_st account;
    account.apiName = "chaynsapi";
    account.userName = "fixture-" + type + "-account";
    account.passwd = "unused";
    account.authToken = "fixture-token";
    account.useCount = 0;
    account.tokenStatus = true;
    account.accountStatus = true;
    account.userTobitId = 1001;
    account.personId = "<user-person>";
    account.createTime = "2026-01-01 00:00:00";
    account.accountType = type;
    account.status = AccountStatus::ACTIVE;
    account.workspaceUacId = type == "pro" ? 9001 : 0;
    return account;
}

void installAccount(AccountManager& accountManager, const std::string& type)
{
    auto accountStore = std::make_shared<ChaynsFixtureAccountStore>();
    accountStore->rows = {chaynsAccount(type)};
    accountManager.setStore(accountStore);
    accountManager.loadAccount();
}

provider::ProviderRequest makeProviderRequest(
    const std::string& model,
    const std::string& conversationId,
    const std::string& input,
    const std::string& previousConversationId = {})
{
    provider::ProviderRequest request;
    request.conversationId = conversationId;
    request.previousConversationId = previousConversationId;
    request.model = model;
    request.input = input;
    request.rawInput = input;
    request.messages.push_back(provider::ProviderMessage{
        provider::ProviderMessageRole::User, input, {}});
    return request;
}

provider::ProviderCallContext makeProviderContext(
    const platform::CancellationToken& cancellation)
{
    return provider::ProviderCallContext{
        cancellation, std::chrono::steady_clock::now() + std::chrono::minutes(10)};
}

GenerationRequest makeGenerationRequest(EndpointType endpoint,
                                        bool stream,
                                        const std::string& input)
{
    GenerationRequest request;
    request.endpointType = endpoint;
    request.provider = "chaynsapi";
    request.model = "fixture-free-model";
    request.messages = {Message::user(input)};
    request.currentInput = input;
    request.continuityTexts = {input};
    request.toolChoice = "none";
    request.stream = stream;
    request.requestId = "fixture-request-" + input;
    return request;
}

struct GenerationFixtureHarness
{
    GenerationFixtureHarness()
    {
        installAccount(accounts, "free");
        transport = std::make_shared<FixtureChaynsTransport>();
        transport->enqueue("model-catalog.json");
        transport->enqueue("thread-create-free.json");
        transport->enqueue("poll-messages.json");
        provider = std::make_shared<chaynsapi>(
            accounts, transport, chayns::makeRealChaynsClock());
        const auto initialization = provider->initialize();
        if (!initialization) {
            throw std::runtime_error("failed to initialize chayns fixture provider");
        }
        if (!registry.registerChatProvider("chaynsapi", provider, provider, provider)) {
            throw std::runtime_error("failed to register chayns fixture provider");
        }
    }

    AccountManager accounts;
    std::shared_ptr<FixtureChaynsTransport> transport;
    std::shared_ptr<chaynsapi> provider;
    provider::ProviderRegistry registry;
    chatSession sessionStore;
    ResponseIndex responseIndex;
    session::SessionExecutionGate executionGate;
};

}  // namespace

DROGON_TEST(ChaynsProvider_FixtureTransportRunsRealPortPathOffline)
{
    AccountManager accounts;
    installAccount(accounts, "free");

    auto transport = std::make_shared<FixtureChaynsTransport>();
    transport->enqueue("model-catalog.json");
    transport->enqueue("thread-create-free.json");
    transport->enqueue("poll-empty.json");
    transport->enqueue("poll-messages.json");

    chaynsapi provider(accounts, transport, chayns::makeRealChaynsClock());
    REQUIRE(provider.initialize().ok());
    platform::CancellationSource cancellation;
    const auto token = cancellation.token();
    auto context = makeProviderContext(token);
    const auto result = provider.generate(
        makeProviderRequest("fixture-free-model", "fixture-conversation", "synthetic user question"),
        context);

    REQUIRE(result.ok());
    CHECK(result.value().text == "synthetic final answer");
    CHECK(result.value().meta.at("chayns.request_message_id") == "<request-message>");
    CHECK(result.value().meta.at("chayns.assistant_message_id") == "<message-3>");
    CHECK(result.value().meta.at("chayns.reasoning_messages").find("<message-2>") != std::string::npos);
    CHECK(transport->remaining() == 0);
    CHECK(transport->errors.empty());
    CHECK(transport->calledPaths.size() == 4);
}

DROGON_TEST(ChaynsProvider_ProFixturePreservesWorkspaceRouteOffline)
{
    AccountManager accounts;
    installAccount(accounts, "pro");
    auto transport = std::make_shared<FixtureChaynsTransport>();
    transport->enqueue("model-catalog.json");
    transport->enqueue("thread-create-pro.json");
    transport->enqueue("poll-messages.json");

    chaynsapi provider(accounts, transport, chayns::makeRealChaynsClock());
    REQUIRE(provider.initialize().ok());
    platform::CancellationSource cancellation;
    const auto token = cancellation.token();
    auto context = makeProviderContext(token);
    const auto result = provider.generate(
        makeProviderRequest("fixture-pro-model", "fixture-pro-conversation", "synthetic user question"),
        context);

    REQUIRE(result.ok());
    CHECK(result.value().text == "synthetic final answer");
    CHECK(result.value().meta.at("chayns.account_type") == "pro");
    CHECK(result.value().meta.at("chayns.thread_type_id") == "9");
    CHECK(result.value().meta.at("chayns.workspace_uac_id") == "9001");
    CHECK(transport->remaining() == 0);
    CHECK(transport->errors.empty());
}

DROGON_TEST(ChaynsProvider_FollowupFixtureUsesExplicitPreviousConversationOffline)
{
    AccountManager accounts;
    installAccount(accounts, "free");
    auto transport = std::make_shared<FixtureChaynsTransport>();
    transport->enqueue("model-catalog.json");
    transport->enqueue("thread-create-free.json");
    transport->enqueue("poll-messages.json");
    transport->enqueue("message-create.json");
    transport->enqueue("poll-messages.json");

    chaynsapi provider(accounts, transport, chayns::makeRealChaynsClock());
    REQUIRE(provider.initialize().ok());
    platform::CancellationSource cancellation;
    const auto token = cancellation.token();
    auto firstContext = makeProviderContext(token);
    REQUIRE(provider.generate(
        makeProviderRequest("fixture-free-model", "fixture-first", "synthetic user question"),
        firstContext).ok());

    auto followupContext = makeProviderContext(token);
    const auto result = provider.generate(
        makeProviderRequest("fixture-free-model", "fixture-second", "synthetic follow-up question", "fixture-first"),
        followupContext);

    REQUIRE(result.ok());
    CHECK(result.value().text == "synthetic final answer");
    CHECK(result.value().meta.at("chayns.request_message_id") == "<followup-request-message>");
    REQUIRE(transport->calledPaths.size() == 5);
    CHECK(transport->calledPaths[3] ==
          "/intercom-backend/v2/thread/<thread-1>/message");
    CHECK(transport->remaining() == 0);
    CHECK(transport->errors.empty());
}

DROGON_TEST(ChaynsProvider_FakeClockReachesPollingDeadlineWithoutWallClockWait)
{
    AccountManager accounts;
    installAccount(accounts, "free");
    auto transport = std::make_shared<FixtureChaynsTransport>();
    transport->enqueue("model-catalog.json");
    transport->enqueue("thread-create-free.json");
    transport->enqueue("poll-empty.json");
    auto clock = std::make_shared<DeadlineJumpClock>();

    chaynsapi provider(accounts, transport, clock);
    REQUIRE(provider.initialize().ok());
    platform::CancellationSource cancellation;
    const auto token = cancellation.token();
    auto context = makeProviderContext(token);
    const auto result = provider.generate(
        makeProviderRequest("fixture-free-model", "fixture-timeout", "synthetic user question"),
        context);

    CHECK(!result.ok());
    CHECK(result.error().code == platform::ErrorCode::Timeout);
    CHECK(result.error().providerCode == "upstream_response_timeout");
    CHECK(clock->sleepCalls == 1);
    REQUIRE(clock->requestedSleeps.size() == 1);
    CHECK(clock->requestedSleeps[0] == std::chrono::milliseconds(200));
    CHECK(transport->remaining() == 0);
    CHECK(transport->errors.empty());
}

DROGON_TEST(ChaynsProvider_CancellationStopsBeforeTheNextPollingBoundary)
{
    AccountManager accounts;
    installAccount(accounts, "free");
    auto transport = std::make_shared<FixtureChaynsTransport>();
    transport->enqueue("model-catalog.json");
    transport->enqueue("thread-create-free.json");
    transport->enqueue("poll-messages.json");

    chaynsapi provider(accounts, transport, chayns::makeRealChaynsClock());
    REQUIRE(provider.initialize().ok());

    platform::CancellationSource cancellation;
    transport->afterSend = [&cancellation](const drogon::HttpRequestPtr& request) {
        if (request && request->method() == drogon::HttpMethod::Post &&
            request->getPath().find("/intercom-backend/v2/thread") != std::string::npos) {
            cancellation.request();
        }
    };
    const auto token = cancellation.token();
    auto context = makeProviderContext(token);
    const auto result = provider.generate(
        makeProviderRequest("fixture-free-model", "fixture-cancelled", "synthetic user question"),
        context);

    CHECK(!result.ok());
    CHECK(result.error().code == platform::ErrorCode::Cancelled);
    CHECK(transport->calledPaths.size() == 2);
    CHECK(transport->remaining() == 1);
    CHECK(transport->errors.empty());
}

DROGON_TEST(ChaynsProvider_ChatJsonRunsGenerationServiceAndProductionSink)
{
    GenerationFixtureHarness harness;
    Json::Value body;
    int status = 0;
    ChatJsonSink sink(
        [&](const Json::Value& value, int valueStatus) {
            body = value;
            status = valueStatus;
        },
        "fixture-free-model");
    GenerationService service(&harness.registry, &harness.sessionStore, &harness.responseIndex, &harness.executionGate);
    const auto error = service.runGuarded(
        makeGenerationRequest(EndpointType::ChatCompletions, false, "chat-json-input"),
        sink);

    CHECK(!error.has_value());
    CHECK(status == 200);
    CHECK(body["object"].asString() == "chat.completion");
    CHECK(body["choices"][0]["message"]["content"].asString() ==
          "synthetic final answer");
    CHECK(harness.transport->remaining() == 0);
    CHECK(harness.transport->errors.empty());
}

DROGON_TEST(ChaynsProvider_ChatSseRunsGenerationServiceAndProductionSink)
{
    GenerationFixtureHarness harness;
    std::string chunks;
    int closes = 0;
    ChatSseSink sink(
        [&](const std::string& chunk) {
            chunks += chunk;
            return true;
        },
        [&]() { ++closes; },
        "fixture-free-model");
    GenerationService service(&harness.registry, &harness.sessionStore, &harness.responseIndex, &harness.executionGate);
    const auto error = service.runGuarded(
        makeGenerationRequest(EndpointType::ChatCompletions, true, "chat-sse-input"),
        sink);

    CHECK(!error.has_value());
    CHECK(chunks.find("synthetic final answer") != std::string::npos);
    CHECK(chunks.find("[DONE]") != std::string::npos);
    CHECK(closes == 1);
    CHECK(harness.transport->remaining() == 0);
    CHECK(harness.transport->errors.empty());
}

DROGON_TEST(ChaynsProvider_ResponsesJsonRunsGenerationServiceAndProductionSink)
{
    GenerationFixtureHarness harness;
    Json::Value body;
    int status = 0;
    ResponsesJsonSink sink(
        [&](const Json::Value& value, int valueStatus) {
            body = value;
            status = valueStatus;
        },
        "fixture-free-model",
        0,
        false);
    GenerationService service(&harness.registry, &harness.sessionStore, &harness.responseIndex, &harness.executionGate);
    const auto error = service.runGuarded(
        makeGenerationRequest(EndpointType::Responses, false, "responses-json-input"),
        sink);

    CHECK(!error.has_value());
    CHECK(status == 200);
    CHECK(body["object"].asString() == "response");
    REQUIRE(body["output"].isArray());
    CHECK(body["output"][0]["content"][0]["text"].asString() ==
          "synthetic final answer");
    CHECK(harness.transport->remaining() == 0);
    CHECK(harness.transport->errors.empty());
}

DROGON_TEST(ChaynsProvider_ResponsesSseRunsGenerationServiceAndProductionSink)
{
    GenerationFixtureHarness harness;
    std::string chunks;
    int closes = 0;
    ResponsesSseSink sink(
        [&](const std::string& chunk) {
            chunks += chunk;
            return true;
        },
        [&]() { ++closes; },
        "fixture-free-model",
        false);
    GenerationService service(&harness.registry, &harness.sessionStore, &harness.responseIndex, &harness.executionGate);
    const auto error = service.runGuarded(
        makeGenerationRequest(EndpointType::Responses, true, "responses-sse-input"),
        sink);

    CHECK(!error.has_value());
    CHECK(chunks.find("response.created") != std::string::npos);
    CHECK(chunks.find("synthetic final answer") != std::string::npos);
    CHECK(chunks.find("response.completed") != std::string::npos);
    CHECK(closes == 1);
    CHECK(harness.transport->remaining() == 0);
    CHECK(harness.transport->errors.empty());
}

DROGON_TEST(ChaynsProvider_GenerationServicePreservesProviderErrorCodeForTransport)
{
    GenerationFixtureHarness harness;
    Json::Value body;
    int status = 0;
    ChatJsonSink sink(
        [&](const Json::Value& value, int valueStatus) {
            body = value;
            status = valueStatus;
        },
        "missing-fixture-model");
    auto request = makeGenerationRequest(
        EndpointType::ChatCompletions, false, "unknown-model-input");
    request.model = "missing-fixture-model";

    GenerationService service(&harness.registry, &harness.sessionStore,
                              &harness.responseIndex, &harness.executionGate);
    const auto error = service.runGuarded(request, sink);

    CHECK(!error.has_value());
    CHECK(status == 400);
    CHECK(body["error"]["type"].asString() == "bad_request");
    CHECK(body["error"]["message"].asString().find("Unknown model") != std::string::npos);
    // `initialize()` loaded the catalog; the forced refresh is rate-limited,
    // so a semantic request error must not invoke create/poll exchanges.
    CHECK(harness.transport->calledPaths.size() == 1);
    CHECK(harness.transport->errors.empty());
}
