#include <drogon/drogon_test.h>

// ARCH_TESTS: infrastructure/provider/retool/RetoolAgentClient.h
// ARCH_TESTS: infrastructure/provider/retool/RetoolProtocolHttp.h
// ARCH_TESTS: infrastructure/provider/retool/RetoolProvider.h

#include <infrastructure/provider/retool/RetoolClock.h>
#include <infrastructure/provider/retool/RetoolHttpTransport.h>
#include <infrastructure/provider/retool/RetoolWorkspaceContext.h>
#include <infrastructure/provider/retool/retoolapi.h>
#include <infrastructure/provider/retool/RetoolProviderSettings.h>
#include <infrastructure/provider/retool/RetoolWorkflowClient.h>
#include <application/workspace/RetoolWorkspaceUseCase.h>
#include <domain/port/IRetoolWorkspaceStore.h>
#include <infrastructure/managedAccount/backends/RetoolWorkspaceBackend.h>
#include <infrastructure/managedAccount/service/ManagedAccountService.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

Json::Value loadFixture(const std::string& name)
{
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "retool" / name;
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open fixture: " + path.string());
    Json::CharReaderBuilder builder;
    Json::Value value;
    std::string errors;
    if (!Json::parseFromStream(builder, input, &value, &errors))
        throw std::runtime_error("invalid fixture: " + errors);
    return value;
}

drogon::HttpMethod methodFromString(const std::string& value)
{
    if (value == "GET") return drogon::HttpMethod::Get;
    if (value == "POST") return drogon::HttpMethod::Post;
    throw std::runtime_error("unsupported Retool fixture method: " + value);
}

struct Exchange
{
    std::string baseUrl;
    drogon::HttpMethod method;
    std::string path;
    Json::Value request{Json::objectValue};
    Json::Value response{Json::objectValue};
};

class FixtureRetoolTransport final : public retool::IRetoolHttpTransport
{
  public:
    void enqueue(const Json::Value& fixture)
    {
        const auto entries = fixture.isMember("exchanges") ? fixture["exchanges"]
                                                               : Json::Value(Json::arrayValue);
        for (const auto& entry : entries)
        {
            Exchange exchange;
            exchange.baseUrl = entry["request"].get("base_url", "https://workspace.invalid").asString();
            exchange.method = methodFromString(entry["request"]["method"].asString());
            exchange.path = entry["request"]["path"].asString();
            exchange.request = entry["request"];
            exchange.response = entry["response"];
            exchanges_.push_back(std::move(exchange));
        }
    }

    void enqueueSingle(const Json::Value& entry)
    {
        Json::Value wrapper(Json::objectValue);
        wrapper["exchanges"] = Json::Value(Json::arrayValue);
        wrapper["exchanges"].append(entry);
        enqueue(wrapper);
    }

    retool::HttpResult send(const std::string& baseUrl,
                            const drogon::HttpRequestPtr& request,
                            double timeoutSeconds) override
    {
        if (!request)
        {
            errors.push_back("null request");
            return {drogon::ReqResult::BadResponse, nullptr};
        }
        if (exchanges_.empty())
        {
            errors.push_back("unexpected request: " + request->getPath());
            return {drogon::ReqResult::BadResponse, nullptr};
        }
        const auto exchange = exchanges_.front();
        exchanges_.pop_front();
        calledBaseUrls.push_back(baseUrl);
        calledPaths.push_back(request->getPath());
        if (baseUrl != exchange.baseUrl) errors.push_back("base URL mismatch");
        if (request->method() != exchange.method) errors.push_back("method mismatch: " + request->getPath());
        if (request->getPath() != exchange.path) errors.push_back("path mismatch: " + request->getPath());
        if (timeoutSeconds <= 0.0) errors.push_back("non-positive timeout");
        for (const auto& header : exchange.request.get("headers_present", Json::Value(Json::arrayValue)))
        {
            if (request->getHeader(header.asString()).empty())
                errors.push_back("missing header: " + header.asString());
        }
        if (exchange.request.isMember("body_fields"))
        {
            const auto body = request->getJsonObject();
            if (!body) errors.push_back("missing JSON body: " + request->getPath());
            else
            {
                for (const auto& key : exchange.request["body_fields"].getMemberNames())
                {
                    if ((*body)[key] != exchange.request["body_fields"][key])
                        errors.push_back("body field mismatch: " + key);
                }
            }
        }
        if (exchange.request.isMember("body_contains"))
        {
            const auto body = std::string(request->getBody());
            for (const auto& token : exchange.request["body_contains"])
                if (body.find(token.asString()) == std::string::npos)
                    errors.push_back("body missing token: " + token.asString());
        }

        const auto status = static_cast<drogon::HttpStatusCode>(exchange.response["status"].asInt());
        drogon::HttpResponsePtr response;
        if (exchange.response.isMember("raw_body"))
        {
            response = drogon::HttpResponse::newHttpResponse();
            response->setBody(exchange.response["raw_body"].asString());
        }
        else
        {
            response = drogon::HttpResponse::newHttpJsonResponse(exchange.response.get("body", Json::Value(Json::objectValue)));
        }
        response->setStatusCode(status);
        if (afterSend) afterSend(request->getPath());
        return {drogon::ReqResult::Ok, response};
    }

    std::size_t remaining() const { return exchanges_.size(); }
    std::vector<std::string> errors;
    std::vector<std::string> calledBaseUrls;
    std::vector<std::string> calledPaths;
    std::function<void(const std::string&)> afterSend;

  private:
    std::deque<Exchange> exchanges_;
};

class FakeRetoolClock final : public retool::IRetoolClock
{
  public:
    Clock::time_point now() const override { return now_; }
    void sleepFor(std::chrono::milliseconds duration) override
    {
        ++sleepCalls;
        slept.push_back(duration);
        now_ += duration;
    }
    int sleepCalls = 0;
    std::vector<std::chrono::milliseconds> slept;
  private:
    Clock::time_point now_{Clock::time_point{}};
};

class FakeRetoolWorkspaceStore final : public IRetoolWorkspaceStore
{
  public:
    std::vector<RetoolWorkspaceInfo> rows;
    std::vector<std::string> usageLog;

    bool ensureTable(std::string*) override { return true; }
    bool upsertWorkspace(const RetoolWorkspaceInfo& info, std::string*) override
    {
        for (auto& row : rows)
        {
            if (row.workspaceId == info.workspaceId) { row = info; return true; }
        }
        rows.push_back(info);
        return true;
    }
    bool deleteWorkspace(const std::string& id, std::string*) override
    {
        const auto old = rows.size();
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const auto& row) { return row.workspaceId == id; }), rows.end());
        return old != rows.size();
    }
    std::optional<RetoolWorkspaceInfo> getWorkspace(const std::string& id, std::string*) override
    {
        for (const auto& row : rows) if (row.workspaceId == id) return row;
        return std::nullopt;
    }
    std::vector<RetoolWorkspaceInfo> listWorkspaces(std::string*) override { return rows; }
    bool updateWorkspaceStatus(const std::string& id, const std::string& status,
                               const std::string& verifyStatus, std::string*) override
    {
        for (auto& row : rows)
        {
            if (row.workspaceId == id) { row.status = status; row.verifyStatus = verifyStatus; return true; }
        }
        return false;
    }
    bool updateWorkspaceUsage(const std::string& id, int count, bool touch, std::string*) override
    {
        usageLog.push_back(id + ":" + std::to_string(count) + (touch ? ":touch" : ":notouch"));
        for (auto& row : rows) if (row.workspaceId == id) { row.inUseCount = count; return true; }
        return false;
    }
};

class NoopManagedAccountBackend final : public IManagedAccountBackend
{
  public:
    ManagedAccountKind kind() const override
    { return ManagedAccountKind::ClassicProviderAccount; }
    std::vector<ManagedAccountRecord> list() override { return {}; }
    std::optional<ManagedAccountRecord> get(const std::string&) override
    { return std::nullopt; }
    bool disable(const std::string&, std::string*) override { return false; }
    std::optional<ManagedExecutionContext> buildExecutionContext(
        const std::string&, std::string*) override
    { return std::nullopt; }
};

class RetoolProviderFixture
{
  public:
    RetoolProviderFixture(const std::string& id,
                          bool workflow,
                          bool agent,
                          const std::string& openai = "openai-resource-synthetic",
                          const std::string& anthropic = "anthropic-resource-synthetic")
        : workspaces(store.get(), nullptr),
          retoolBackend(std::make_shared<RetoolWorkspaceBackend>(workspaces)),
          accounts(std::make_shared<NoopManagedAccountBackend>(), retoolBackend)
    {
        RetoolWorkspaceInfo info;
        info.workspaceId = id;
        info.baseUrl = "https://workspace.invalid";
        info.workflowId = workflow ? "workflow-1" : "";
        info.agentId = agent ? "agent-1" : "";
        info.openaiResourceName = openai;
        info.anthropicResourceName = anthropic;
        info.accessToken = "synthetic-access-token";
        info.xsrfToken = "synthetic-xsrf-token";
        info.status = "active";
        info.verifyStatus = "passed";
        store->rows.push_back(info);
    }

    retoolapi provider(std::shared_ptr<retool::IRetoolHttpTransport> transport,
                       std::shared_ptr<retool::IRetoolClock> clock)
    {
        return retoolapi(std::move(transport), std::move(clock),
                         accounts, workspaces, channels);
    }

    std::shared_ptr<FakeRetoolWorkspaceStore> store =
        std::make_shared<FakeRetoolWorkspaceStore>();
    workspace::RetoolWorkspaceUseCase workspaces;
    std::shared_ptr<RetoolWorkspaceBackend> retoolBackend;
    ManagedAccountService accounts;

  private:
    class Channels final : public IChannelCatalog
    {
      public:
        std::list<Channelinfo_st> listChannels() const override { return {}; }
        bool addChannel(Channelinfo_st) override { return false; }
        bool updateChannel(Channelinfo_st) override { return false; }
        bool deleteChannel(int) override { return false; }
        bool updateChannelStatus(std::string, bool) override { return false; }
        std::optional<bool> supportsToolCalls(const std::string&) const override
        { return std::nullopt; }
    } channels;
};

RetoolProviderFixture installWorkspace(const std::string& id,
                                       bool workflow,
                                       bool agent,
                                       const std::string& openai = "openai-resource-synthetic",
                                       const std::string& anthropic = "anthropic-resource-synthetic")
{
    return RetoolProviderFixture(id, workflow, agent, openai, anthropic);
}

provider::ProviderRequest makeRequest(const std::string& model,
                                      const std::string& conversation,
                                      const std::string& message,
                                      const std::string& workspace = "ws-1")
{
    provider::ProviderRequest request;
    request.model = model;
    request.input = message;
    request.rawInput = message;
    request.conversationId = conversation;
    if (!workspace.empty()) request.routingHints["workspace_id"] = workspace;
    return request;
}

provider::ProviderCallContext makeProviderContext(
    const platform::CancellationToken& token,
    std::chrono::milliseconds budget = std::chrono::minutes(10))
{
    return provider::ProviderCallContext{
        token, std::chrono::steady_clock::now() + budget};
}

platform::Result<provider::ProviderResponse> invoke(
    retoolapi& provider, const provider::ProviderRequest& request)
{
    const auto initialized = provider.initialize();
    if (!initialized) {
        return platform::Result<provider::ProviderResponse>::failure(
            initialized.error());
    }
    platform::CancellationSource cancellation;
    const auto token = cancellation.token();
    auto context = makeProviderContext(token);
    return provider.generate(request, context);
}

bool usageBalanced(const std::shared_ptr<FakeRetoolWorkspaceStore>& store)
{
    return store->usageLog.size() == 2 &&
           store->usageLog[0] == "ws-1:1:touch" &&
           store->usageLog[1] == "ws-1:0:touch";
}

void enqueueAgentSetup(const std::shared_ptr<FixtureRetoolTransport>& transport)
{
    const auto fixture = loadFixture("agent-success.json");
    for (const auto& exchange : fixture["exchanges"])
    {
        if (exchange["request"]["path"].asString().find("/logs/") == std::string::npos)
            transport->enqueueSingle(exchange);
    }
}

Json::Value agentPollExchange(const std::string& status)
{
    Json::Value exchange(Json::objectValue);
    exchange["request"]["base_url"] = "https://workspace.invalid";
    exchange["request"]["method"] = "GET";
    exchange["request"]["path"] =
        "/api/agents/agent-1/logs/agent-run-1?startAfterUUID=00000000-0000-7000-8000-000000000000&limit=100";
    exchange["response"]["status"] = 200;
    exchange["response"]["body"]["status"] = status;
    exchange["response"]["body"]["trace"] = Json::Value(Json::arrayValue);
    return exchange;
}

}  // namespace

DROGON_TEST(RetoolWorkspaceContext_OwnsAffinityThreadsAndUsageLease)
{
    auto store = std::make_shared<FakeRetoolWorkspaceStore>();
    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-context";
    info.baseUrl = "https://workspace.invalid";
    info.status = "active";
    info.verifyStatus = "passed";
    store->rows.push_back(info);
    workspace::RetoolWorkspaceUseCase workspaces(store.get(), nullptr);
    retool::RetoolWorkspaceContext context(workspaces);

    context.bindWorkspace("conversation-before", "ws-context");
    context.bindAgentThread("conversation-before", "thread-context");
    CHECK(context.workspaceFor("conversation-before") == "ws-context");
    CHECK(context.agentThreadFor("conversation-before") == "thread-context");
    {
        auto lease = context.startUsage("ws-context");
        CHECK(store->usageLog.size() == 1);
        CHECK(store->usageLog[0] == "ws-context:1:touch");
    }
    REQUIRE(store->usageLog.size() == 2);
    CHECK(store->usageLog[1] == "ws-context:0:touch");

    REQUIRE(context.transfer("conversation-before", "conversation-after").ok());
    CHECK(context.workspaceFor("conversation-after") == "ws-context");
    CHECK(context.agentThreadFor("conversation-after") == "thread-context");
    REQUIRE(context.erase("conversation-after").ok());
    CHECK(context.workspaceFor("conversation-after").empty());
    CHECK(context.agentThreadFor("conversation-after").empty());
}

DROGON_TEST(RetoolProviderSettings_ParsesInjectedLimitsWithoutGlobalConfig)
{
    Json::Value config(Json::objectValue);
    config["retoolapi"]["agent_bootstrap_system_prompt_max_chars"] = 0;
    config["history_replay"]["max_request_bytes"] = 99 * 1024 * 1024;
    config["history_replay"]["max_message_bytes"] = -1;

    const auto settings = retool::providerSettingsFromConfig(config);
    CHECK(settings.agentBootstrapSystemPromptMaxChars == 0);
    CHECK(settings.historyReplayMaxRequestBytes == 8 * 1024 * 1024);
    CHECK(settings.historyReplayMaxMessageBytes == 128 * 1024);
}

DROGON_TEST(RetoolProtocolClient_MapsTransportCancellationAndFailure)
{
    auto transport = std::make_shared<FixtureRetoolTransport>();
    retool::RetoolWorkflowClient client(transport);

    platform::CancellationSource cancellation;
    auto token = cancellation.token();
    auto context = makeProviderContext(token);
    const auto failed = client.fetchWorkflow(
        context, "https://workspace.invalid", "workflow-1", Json::Value(Json::objectValue));
    REQUIRE(!failed);
    CHECK(failed.error().code == platform::ErrorCode::ProviderError);
    CHECK(failed.error().message.find("fetch workflow") != std::string::npos);

    cancellation.request();
    const auto cancelled = client.fetchWorkflow(
        context, "https://workspace.invalid", "workflow-1", Json::Value(Json::objectValue));
    REQUIRE(!cancelled);
    CHECK(cancelled.error().code == platform::ErrorCode::Cancelled);
}

DROGON_TEST(RetoolProtocolHttp_DecodeJsonBodyRejectsMalformedPayload)
{
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k200OK);
    response->setBody("{not-json");

    const auto decoded = retool::protocol_http::decodeJsonBody(
        response, "fetch workflow");
    REQUIRE(!decoded.ok());
    CHECK(decoded.error().code == platform::ErrorCode::ProviderError);
    CHECK(decoded.error().providerCode == "invalid_json");
    CHECK(decoded.error().upstreamHttpStatus == 200);
}

DROGON_TEST(RetoolProvider_WorkflowFixtureRunsRealRequestWorkflowOffline)
{
    auto fixture = installWorkspace("ws-1", true, false);
    auto transport = std::make_shared<FixtureRetoolTransport>();
    transport->enqueue(loadFixture("workflow-success.json"));
    auto clock = std::make_shared<FakeRetoolClock>();
    auto provider = fixture.provider(transport, clock);

    auto request = makeRequest("gpt-4o-mini", "workflow-conversation", "synthetic current question");
    const auto result = invoke(provider, request);

    REQUIRE(result.ok());
    CHECK(result.value().text == "synthetic workflow answer");
    CHECK(result.value().meta.at("routeType") == "workflow");
    CHECK(result.value().meta.at("workspaceId") == "ws-1");
    CHECK(result.value().meta.at("provider") == "openAI");
    CHECK(transport->remaining() == 0);
    CHECK(transport->errors.empty());
    CHECK(clock->sleepCalls == 1);
    CHECK(usageBalanced(fixture.store));
}

DROGON_TEST(RetoolProvider_AgentFixtureRunsRealRequestAgentOffline)
{
    auto fixture = installWorkspace("ws-1", false, true);
    auto transport = std::make_shared<FixtureRetoolTransport>();
    transport->enqueue(loadFixture("agent-success.json"));
    auto clock = std::make_shared<FakeRetoolClock>();
    auto provider = fixture.provider(transport, clock);

    auto request = makeRequest("agent-claude-sonnet-4-6", "agent-conversation", "synthetic agent question");
    const auto result = invoke(provider, request);

    REQUIRE(result.ok());
    CHECK(result.value().text == "synthetic agent answer");
    CHECK(result.value().meta.at("routeType") == "agent");
    CHECK(result.value().meta.at("resourceId") == "agent-1");
    CHECK(result.value().meta.at("model") == "claude-sonnet-4-6");
    CHECK(result.value().meta.at("provider") == "anthropic");
    CHECK(transport->remaining() == 0);
    CHECK(transport->errors.empty());
    CHECK(clock->sleepCalls == 1);
    CHECK(usageBalanced(fixture.store));
}

DROGON_TEST(RetoolProvider_ProvidesNarrowModelAndThreadCapabilities)
{
    auto fixture = installWorkspace("ws-1", true, true);
    auto provider = fixture.provider(
        std::make_shared<FixtureRetoolTransport>(), std::make_shared<FakeRetoolClock>());

    REQUIRE(provider.initialize().ok());
    const auto catalog = provider.getModels();
    CHECK(std::any_of(catalog.models.begin(), catalog.models.end(), [](const ProviderModel& model) {
        return model.id == "gpt-4o-mini";
    }));
    CHECK(std::any_of(catalog.models.begin(), catalog.models.end(), [](const ProviderModel& model) {
        return model.id == "agent-claude-sonnet-4-6";
    }));
    CHECK(provider.capabilities().upstreamHistory);
    CHECK(provider.transferThreadContext("old-conversation", "new-conversation").ok());
    CHECK(provider.eraseThreadContext("new-conversation").ok());

    const auto upstreamDelete = provider.deleteUpstreamThread(
        "unused-account", "unused-thread", "", "");
    REQUIRE(!upstreamDelete.ok());
    CHECK(upstreamDelete.error().code == platform::ErrorCode::NotFound);
}

DROGON_TEST(RetoolProvider_WorkspaceAffinityPersistsAfterExplicitSelection)
{
    auto fixture = installWorkspace("ws-1", true, false);
    auto transport = std::make_shared<FixtureRetoolTransport>();
    const auto workflowFixture = loadFixture("workflow-success.json");
    transport->enqueue(workflowFixture);
    transport->enqueue(workflowFixture);
    auto provider = fixture.provider(transport, std::make_shared<FakeRetoolClock>());

    auto first = makeRequest("gpt-4o-mini", "affinity-conversation", "synthetic current question");
    CHECK(invoke(provider, first).ok());
    auto second = makeRequest("gpt-4o-mini", "affinity-conversation", "synthetic current question");
    second.routingHints.erase("workspace_id");
    const auto secondResult = invoke(provider, second);
    REQUIRE(secondResult.ok());
    CHECK(secondResult.value().meta.at("workspace_id") == "ws-1");
    CHECK(transport->errors.empty());
    CHECK(fixture.store->usageLog.size() == 4);
}

DROGON_TEST(RetoolProvider_WorkflowErrorMappingsComeFromProductionPath)
{
    const auto errors = loadFixture("http-errors.json")["cases"];
    for (const auto& item : errors)
    {
        auto fixture = installWorkspace("ws-1", true, false);
        auto transport = std::make_shared<FixtureRetoolTransport>();
        Json::Value entry(Json::objectValue);
        entry["request"]["base_url"] = "https://workspace.invalid";
        entry["request"]["method"] = "GET";
        entry["request"]["path"] = "/api/workflow/workflow-1";
        entry["response"] = item;
        transport->enqueueSingle(entry);
        auto provider = fixture.provider(transport, std::make_shared<FakeRetoolClock>());
        auto request = makeRequest("gpt-4o-mini", "error-" + std::to_string(item["status"].asInt()), "question");
        const auto result = invoke(provider, request);
        REQUIRE(!result.ok());
        CHECK(result.error().upstreamHttpStatus == item["status"].asInt());
        if (item["expected_code"] == "AuthError") CHECK(result.error().code == platform::ErrorCode::Unauthorized);
        if (item["expected_code"] == "RateLimited") CHECK(result.error().code == platform::ErrorCode::RateLimited);
        if (item["expected_code"] == "InternalError") CHECK(result.error().code == platform::ErrorCode::ProviderError);
        CHECK(usageBalanced(fixture.store));
    }
}

DROGON_TEST(RetoolProvider_InvalidJsonMappingIsCharacterized)
{
    auto fixture = installWorkspace("ws-1", true, false);
    auto transport = std::make_shared<FixtureRetoolTransport>();
    transport->enqueueSingle(loadFixture("invalid-json.json")["exchange"]);
    auto provider = fixture.provider(transport, std::make_shared<FakeRetoolClock>());
    auto request = makeRequest("gpt-4o-mini", "invalid-json-conversation", "question");
    const auto result = invoke(provider, request);
    REQUIRE(!result.ok());
    CHECK(result.error().code == platform::ErrorCode::ProviderError);
    CHECK(result.error().providerCode == "invalid_json");
    CHECK(result.error().upstreamHttpStatus == 200);
    CHECK(transport->errors.empty());
    CHECK(usageBalanced(fixture.store));
}

DROGON_TEST(RetoolProvider_WorkflowPollingTimeoutUsesFakeClock)
{
    auto fixture = installWorkspace("ws-1", true, false);
    auto transport = std::make_shared<FixtureRetoolTransport>();
    const auto success = loadFixture("workflow-success.json");
    for (const auto& exchange : success["exchanges"])
    {
        if (exchange["request"]["path"].asString().find("getBlockLevelLogs") == std::string::npos)
            transport->enqueueSingle(exchange);
    }
    Json::Value running(Json::objectValue);
    running["request"]["base_url"] = "https://workspace.invalid";
    running["request"]["method"] = "GET";
    running["request"]["path"] = "/api/workflowRun/getBlockLevelLogs?runId=workflow-run-1";
    running["response"]["status"] = 200;
    running["response"]["body"]["blockLevelLogs"]["code1"]["status"] = "RUNNING";
    for (int i = 0; i < 120; ++i) transport->enqueueSingle(running);
    auto clock = std::make_shared<FakeRetoolClock>();
    auto provider = fixture.provider(transport, clock);
    auto request = makeRequest("gpt-4o-mini", "timeout-conversation", "question");
    const auto result = invoke(provider, request);
    REQUIRE(!result.ok());
    CHECK(result.error().code == platform::ErrorCode::Timeout);
    CHECK(clock->sleepCalls == 120);
    CHECK(transport->remaining() == 0);
    CHECK(usageBalanced(fixture.store));
}

DROGON_TEST(RetoolProvider_CancellationStopsBeforeTheNextPollingBoundary)
{
    auto fixture = installWorkspace("ws-1", true, false);
    auto transport = std::make_shared<FixtureRetoolTransport>();
    transport->enqueue(loadFixture("workflow-success.json"));
    platform::CancellationSource cancellation;
    transport->afterSend = [&cancellation](const std::string& path) {
        if (path.find("getBlockLevelLogs") != std::string::npos) {
            cancellation.request();
        }
    };
    auto clock = std::make_shared<FakeRetoolClock>();
    auto provider = fixture.provider(transport, clock);
    REQUIRE(provider.initialize().ok());
    const auto token = cancellation.token();
    auto context = makeProviderContext(token);
    const auto request = makeRequest(
        "gpt-4o-mini", "cancel-conversation", "synthetic question");
    const auto result = provider.generate(request, context);

    REQUIRE(!result.ok());
    CHECK(result.error().code == platform::ErrorCode::Cancelled);
    // The second poll exchange stays queued: cancellation observed immediately
    // after the first blocking boundary prevents another upstream request.
    CHECK(transport->remaining() == 1);
    CHECK(clock->sleepCalls == 0);
    CHECK(usageBalanced(fixture.store));
}

DROGON_TEST(RetoolProvider_AgentFailedRunIsMappedToInternalError)
{
    auto fixture = installWorkspace("ws-1", false, true);
    auto transport = std::make_shared<FixtureRetoolTransport>();
    enqueueAgentSetup(transport);
    auto failed = agentPollExchange("FAILED");
    failed["response"]["body"]["trace"].append(
        Json::Value(Json::objectValue));
    failed["response"]["body"]["trace"][0]["data"]["error"] = "synthetic agent failure";
    transport->enqueueSingle(failed);
    auto provider = fixture.provider(transport, std::make_shared<FakeRetoolClock>());
    auto request = makeRequest("agent-claude-sonnet-4-6", "agent-failed-conversation", "synthetic agent question");
    const auto result = invoke(provider, request);
    REQUIRE(!result.ok());
    CHECK(result.error().code == platform::ErrorCode::ProviderError);
    CHECK(result.error().message == "synthetic agent failure");
    CHECK(transport->remaining() == 0);
    CHECK(transport->errors.empty());
    CHECK(usageBalanced(fixture.store));
}

DROGON_TEST(RetoolProvider_AgentPollingTimeoutUsesFakeClock)
{
    auto fixture = installWorkspace("ws-1", false, true);
    auto transport = std::make_shared<FixtureRetoolTransport>();
    enqueueAgentSetup(transport);
    const auto running = agentPollExchange("RUNNING");
    for (int i = 0; i < 180; ++i) transport->enqueueSingle(running);
    auto clock = std::make_shared<FakeRetoolClock>();
    auto provider = fixture.provider(transport, clock);
    auto request = makeRequest("agent-claude-sonnet-4-6", "agent-timeout-conversation", "synthetic agent question");
    const auto result = invoke(provider, request);
    REQUIRE(!result.ok());
    CHECK(result.error().code == platform::ErrorCode::Timeout);
    CHECK(clock->sleepCalls == 180);
    CHECK(transport->remaining() == 0);
    CHECK(transport->errors.empty());
    CHECK(usageBalanced(fixture.store));
}
