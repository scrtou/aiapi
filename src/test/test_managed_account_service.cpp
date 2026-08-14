#include <drogon/drogon_test.h>

#include <managedAccount/service/ManagedAccountService.h>

namespace {

class RecordingManagedBackend final : public IManagedAccountBackend
{
  public:
    explicit RecordingManagedBackend(ManagedAccountKind backendKind)
        : backendKind_(backendKind)
    {
        record.id = backendKind == ManagedAccountKind::RetoolWorkspace
                        ? "workspace-1" : "chaynsapi:user";
        record.kind = backendKind;
        record.provider = backendKind == ManagedAccountKind::RetoolWorkspace
                              ? "retool" : "chaynsapi";
        record.displayName = record.id;
        record.status = "active";
    }

    ManagedAccountKind kind() const override { return backendKind_; }
    std::vector<ManagedAccountRecord> list() override
    {
        ++listCalls;
        return {record};
    }
    std::optional<ManagedAccountRecord> get(const std::string& id) override
    {
        ++getCalls;
        return id == record.id ? std::optional<ManagedAccountRecord>(record)
                               : std::nullopt;
    }
    bool disable(const std::string& id, std::string*) override
    {
        ++disableCalls;
        return id == record.id;
    }
    std::optional<ManagedExecutionContext> buildExecutionContext(
        const std::string& id, std::string*) override
    {
        ++contextCalls;
        if (id != record.id) return std::nullopt;
        ManagedExecutionContext context;
        context.kind = backendKind_;
        context.id = id;
        context.data["source"] = record.provider;
        return context;
    }

    ManagedAccountRecord record;
    int listCalls = 0;
    int getCalls = 0;
    int disableCalls = 0;
    int contextCalls = 0;

  private:
    ManagedAccountKind backendKind_;
};

}  // namespace

DROGON_TEST(ManagedAccountServiceDispatchesOnlyToInjectedBackend)
{
    auto classic = std::make_shared<RecordingManagedBackend>(
        ManagedAccountKind::ClassicProviderAccount);
    auto retool = std::make_shared<RecordingManagedBackend>(
        ManagedAccountKind::RetoolWorkspace);
    ManagedAccountService service(classic, retool);

    const auto workspace = service.get(
        ManagedAccountKind::RetoolWorkspace, "workspace-1");
    REQUIRE(workspace.has_value());
    CHECK(retool->getCalls == 1);
    CHECK(classic->getCalls == 0);

    std::string error;
    const auto context = service.buildExecutionContext(
        ManagedAccountKind::ClassicProviderAccount, "chaynsapi:user", &error);
    REQUIRE(context.has_value());
    CHECK(context->data["source"] == "chaynsapi");
    CHECK(classic->contextCalls == 1);
    CHECK(retool->contextCalls == 0);
}

DROGON_TEST(ManagedAccountServiceCombinesInjectedBackends)
{
    auto classic = std::make_shared<RecordingManagedBackend>(
        ManagedAccountKind::ClassicProviderAccount);
    auto retool = std::make_shared<RecordingManagedBackend>(
        ManagedAccountKind::RetoolWorkspace);
    ManagedAccountService service(classic, retool);

    const auto records = service.listAll();
    REQUIRE(records.size() == 2);
    CHECK(records[0].kind == ManagedAccountKind::ClassicProviderAccount);
    CHECK(records[1].kind == ManagedAccountKind::RetoolWorkspace);
    CHECK(classic->listCalls == 1);
    CHECK(retool->listCalls == 1);
    CHECK(service.disable(ManagedAccountKind::RetoolWorkspace, "workspace-1"));
    CHECK(retool->disableCalls == 1);
    CHECK(classic->disableCalls == 0);
}
