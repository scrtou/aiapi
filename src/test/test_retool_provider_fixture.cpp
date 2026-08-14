#include <drogon/drogon_test.h>

#include <apipoint/retoolapi/RetoolClock.h>
#include <apipoint/retoolapi/RetoolHttpTransport.h>
#include <apipoint/retoolapi/retoolapi.h>
#include <application/workspace/RetoolWorkspaceUseCase.h>
#include <domain/port/IRetoolWorkspaceStore.h>
#include <managedAccount/backends/RetoolWorkspaceBackend.h>
#include <managedAccount/service/ManagedAccountService.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
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
        return {drogon::ReqResult::Ok, response};
    }

    std::size_t remaining() const { return exchanges_.size(); }
    std::vector<std::string> errors;
    std::vector<std::string> calledBaseUrls;
    std::vector<std::string> calledPaths;

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

session_st makeRequest(const std::string& model, const std::string& conversation, const std::string& message,
                       const std::string& workspace = "ws-1")
{
    session_st session;
    session.request.api = "retoolapi";
    session.request.model = model;
    session.request.message = message;
    session.request.rawMessage = message;
    session.state.conversationId = conversation;
    session.provider.clientInfo["workspace_id"] = workspace;
    return session;
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

DROGON_TEST(RetoolProvider_WorkflowFixtureRunsRealRequestWorkflowOffline)
{
    auto fixture = installWorkspace("ws-1", true, false);
    auto transport = std::make_shared<FixtureRetoolTransport>();
    transport->enqueue(loadFixture("workflow-success.json"));
    auto clock = std::make_shared<FakeRetoolClock>();
    auto provider = fixture.provider(transport, clock);

    auto session = makeRequest("gpt-4o-mini", "workflow-conversation", "synthetic current question");
    const auto result = provider.generate(session);

    CHECK(result.isSuccess());
    CHECK(result.text == "synthetic workflow answer");
    CHECK(result.meta.at("routeType") == "workflow");
    CHECK(result.meta.at("workspaceId") == "ws-1");
    CHECK(result.meta.at("provider") == "openAI");
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

    auto session = makeRequest("agent-claude-sonnet-4-6", "agent-conversation", "synthetic agent question");
    const auto result = provider.generate(session);

    CHECK(result.isSuccess());
    CHECK(result.text == "synthetic agent answer");
    CHECK(result.meta.at("routeType") == "agent");
    CHECK(result.meta.at("resourceId") == "agent-1");
    CHECK(result.meta.at("model") == "claude-sonnet-4-6");
    CHECK(result.meta.at("provider") == "anthropic");
    CHECK(transport->remaining() == 0);
    CHECK(transport->errors.empty());
    CHECK(clock->sleepCalls == 1);
    CHECK(usageBalanced(fixture.store));
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
    CHECK(provider.generate(first).isSuccess());
    auto second = makeRequest("gpt-4o-mini", "affinity-conversation", "synthetic current question");
    second.provider.clientInfo.removeMember("workspace_id");
    CHECK(provider.generate(second).isSuccess());
    CHECK(second.provider.clientInfo["workspace_id"] == "ws-1");
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
        auto session = makeRequest("gpt-4o-mini", "error-" + std::to_string(item["status"].asInt()), "question");
        const auto result = provider.generate(session);
        CHECK(!result.isSuccess());
        CHECK(result.error.httpStatusCode == item["status"].asInt());
        if (item["expected_code"] == "AuthError") CHECK(result.error.code == provider::ProviderErrorCode::AuthError);
        if (item["expected_code"] == "RateLimited") CHECK(result.error.code == provider::ProviderErrorCode::RateLimited);
        if (item["expected_code"] == "InternalError") CHECK(result.error.code == provider::ProviderErrorCode::InternalError);
        CHECK(usageBalanced(fixture.store));
    }
}

DROGON_TEST(RetoolProvider_InvalidJsonMappingIsCharacterized)
{
    auto fixture = installWorkspace("ws-1", true, false);
    auto transport = std::make_shared<FixtureRetoolTransport>();
    transport->enqueueSingle(loadFixture("invalid-json.json")["exchange"]);
    auto provider = fixture.provider(transport, std::make_shared<FakeRetoolClock>());
    auto session = makeRequest("gpt-4o-mini", "invalid-json-conversation", "question");
    const auto result = provider.generate(session);
    CHECK(!result.isSuccess());
    CHECK(result.error.code == provider::ProviderErrorCode::Unknown);
    CHECK(result.statusCode == 200);
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
    auto session = makeRequest("gpt-4o-mini", "timeout-conversation", "question");
    const auto result = provider.generate(session);
    CHECK(!result.isSuccess());
    CHECK(result.error.code == provider::ProviderErrorCode::Timeout);
    CHECK(clock->sleepCalls == 120);
    CHECK(transport->remaining() == 0);
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
    auto session = makeRequest("agent-claude-sonnet-4-6", "agent-failed-conversation", "synthetic agent question");
    const auto result = provider.generate(session);
    CHECK(!result.isSuccess());
    CHECK(result.error.code == provider::ProviderErrorCode::InternalError);
    CHECK(result.error.message == "synthetic agent failure");
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
    auto session = makeRequest("agent-claude-sonnet-4-6", "agent-timeout-conversation", "synthetic agent question");
    const auto result = provider.generate(session);
    CHECK(!result.isSuccess());
    CHECK(result.error.code == provider::ProviderErrorCode::Timeout);
    CHECK(clock->sleepCalls == 180);
    CHECK(transport->remaining() == 0);
    CHECK(transport->errors.empty());
    CHECK(usageBalanced(fixture.store));
}
