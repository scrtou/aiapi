#include <drogon/drogon_test.h>

#include <application/workspace/RetoolWorkspaceUseCase.h>
#include <application/workspace/RetoolWorkspaceAdminUseCase.h>

#include <map>

// ARCH_TESTS: application/workspace/RetoolWorkspaceUseCase.h
// ARCH_TESTS: domain/model/RetoolWorkspaceInfo.h
// ARCH_TESTS: domain/model/ChannelInfo.h
// ARCH_TESTS: domain/port/IKeyValueConfigStore.h
// ARCH_TESTS: domain/port/IRetoolWorkspaceUseCase.h

namespace {

class MemoryWorkspaceStore final : public IRetoolWorkspaceStore
{
  public:
    std::map<std::string, RetoolWorkspaceInfo> data;

    bool ensureTable(std::string*) override { return true; }
    bool upsertWorkspace(const RetoolWorkspaceInfo& info, std::string*) override
    { data[info.workspaceId] = info; return true; }
    bool deleteWorkspace(const std::string& id, std::string*) override
    { return data.erase(id) == 1; }
    std::optional<RetoolWorkspaceInfo> getWorkspace(
        const std::string& id, std::string*) override
    { auto it = data.find(id); return it == data.end() ? std::nullopt : std::optional<RetoolWorkspaceInfo>(it->second); }
    std::vector<RetoolWorkspaceInfo> listWorkspaces(std::string*) override
    { std::vector<RetoolWorkspaceInfo> out; for (auto& item : data) out.push_back(item.second); return out; }
    bool updateWorkspaceStatus(const std::string& id, const std::string& status,
                               const std::string& verify, std::string*) override
    { auto it = data.find(id); if (it == data.end()) return false; it->second.status=status; it->second.verifyStatus=verify; return true; }
    bool updateWorkspaceUsage(const std::string&, int, bool, std::string*) override { return true; }
};

class MemoryConfigStore final : public IKeyValueConfigStore
{
  public:
    std::map<std::string, std::string> values;
    bool ensureTable(std::string*) override { return true; }
    std::optional<std::string> getValue(const std::string& key, std::string*) override
    { auto it=values.find(key); return it==values.end() ? std::nullopt : std::optional<std::string>(it->second); }
    bool setValues(const std::map<std::string,std::string>& input, std::string*) override
    { values.insert(input.begin(), input.end()); return true; }
};

class WorkspaceChannels final : public IChannelCatalog
{
  public:
    std::list<Channelinfo_st> values;
    std::list<Channelinfo_st> listChannels() const override { return values; }
    bool addChannel(Channelinfo_st) override { return false; }
    bool updateChannel(Channelinfo_st) override { return false; }
    bool deleteChannel(int) override { return false; }
    bool updateChannelStatus(std::string, bool) override { return false; }
    std::optional<bool> supportsToolCalls(const std::string&) const override { return std::nullopt; }
};

class RecordingProvisioner final : public workspace::IRetoolWorkspaceProvisioner
{
  public:
    std::string request;

    RetoolWorkspaceInfo provision(const std::string& requestJson) override
    {
        request = requestJson;
        RetoolWorkspaceInfo result;
        result.workspaceId = "provisioned";
        return result;
    }
};

}  // namespace

DROGON_TEST(RetoolWorkspaceUseCasePreservesSecretsOnPartialUpsert)
{
    MemoryWorkspaceStore store;
    MemoryConfigStore config;
    RetoolWorkspaceInfo existing;
    existing.workspaceId = "ws";
    existing.baseUrl = "https://workspace.invalid";
    existing.accessToken = "secret-token";
    store.data[existing.workspaceId] = existing;
    workspace::RetoolWorkspaceUseCase useCase(&store, &config);

    RetoolWorkspaceInfo incoming;
    incoming.workspaceId = "ws";
    incoming.baseUrl = existing.baseUrl;
    REQUIRE(useCase.upsert(incoming, nullptr));
    CHECK(store.data["ws"].accessToken == "secret-token");
}

DROGON_TEST(RetoolWorkspaceUseCaseValidatesMergedBaseUrl)
{
    MemoryWorkspaceStore store;
    MemoryConfigStore config;
    workspace::RetoolWorkspaceUseCase useCase(&store, &config);
    RetoolWorkspaceInfo incoming;
    incoming.workspaceId = "new-workspace";
    std::string error;
    CHECK(!useCase.upsert(incoming, &error));
    CHECK(error == "baseUrl is required");
    CHECK(store.data.empty());
}

DROGON_TEST(RetoolWorkspaceUseCaseBuildsPoolStatusFromInjectedPorts)
{
    MemoryWorkspaceStore store;
    MemoryConfigStore config;
    WorkspaceChannels channels;
    RetoolWorkspaceInfo idle; idle.workspaceId="idle"; idle.status="ready";
    RetoolWorkspaceInfo busy; busy.workspaceId="busy"; busy.status="ready"; busy.inUseCount=2;
    RetoolWorkspaceInfo disabled; disabled.workspaceId="disabled"; disabled.status="disabled";
    store.data[idle.workspaceId]=idle; store.data[busy.workspaceId]=busy; store.data[disabled.workspaceId]=disabled;
    config.values["retoolapi.provision.consecutive_failures"] = "3";
    Channelinfo_st channel; channel.channelName="retoolapi"; channels.values.push_back(channel);
    workspace::RetoolWorkspaceUseCase useCase(&store, &config, &channels);

    const auto status = useCase.poolStatus();
    CHECK(status.total == 3);
    CHECK(status.idle == 1);
    CHECK(status.inUse == 1);
    CHECK(status.disabled == 1);
    CHECK(status.consecutiveFailures == 3);
    REQUIRE(status.channel.has_value());
    CHECK(status.channel->channelName == "retoolapi");
}

DROGON_TEST(RetoolWorkspaceUseCaseVerifyUsesInjectedStore)
{
    MemoryWorkspaceStore store;
    MemoryConfigStore config;
    RetoolWorkspaceInfo item;
    item.workspaceId="ws"; item.status="ready"; item.baseUrl="url";
    item.accessToken="a"; item.xsrfToken="x"; item.workflowId="w"; item.agentId="g";
    store.data[item.workspaceId]=item;
    workspace::RetoolWorkspaceUseCase useCase(&store, &config);

    bool ready=false; std::string verify; RetoolWorkspaceInfo returned;
    REQUIRE(useCase.verify("ws", &ready, &verify, &returned, nullptr));
    CHECK(ready);
    CHECK(verify == "ready");
    CHECK(store.data["ws"].verifyStatus == "ready");
}


DROGON_TEST(RetoolWorkspaceAdminUseCaseCombinesProvisionAndWorkspaceWorkflow)
{
    MemoryWorkspaceStore store;
    MemoryConfigStore config;
    workspace::RetoolWorkspaceUseCase workflows(&store, &config);
    RecordingProvisioner provisioner;
    workspace::RetoolWorkspaceAdminUseCase admin(workflows, provisioner);

    const auto provisioned = admin.provision("{\"email\":\"admin@example.invalid\"}");
    CHECK(provisioner.request == "{\"email\":\"admin@example.invalid\"}");
    CHECK(provisioned.workspaceId == "provisioned");

    RetoolWorkspaceInfo stored;
    stored.workspaceId = "managed";
    stored.baseUrl = "https://workspace.invalid";
    REQUIRE(admin.upsert(stored, nullptr));
    REQUIRE(admin.get("managed", nullptr).has_value());
}
