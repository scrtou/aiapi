#include <drogon/drogon_test.h>

// ARCH_TESTS: domain/model/ProviderCallContext.h

#include <infrastructure/provider/ProductionProviderFactory.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

enum class ProbeMode {
    Success,
    Failure,
    InvalidFailure,
    Throw,
};

class ProbeProvider final : public provider::ProviderBase
{
  public:
    explicit ProbeProvider(ProbeMode mode,
                           FailureObserver observer = {})
        : ProviderBase(std::move(observer)), mode_(mode)
    {
    }

    provider::ProviderCapabilities capabilities() const noexcept override
    {
        return provider::ProviderCapabilities{true, false, true};
    }

    int calls() const noexcept { return calls_; }

  protected:
    platform::Result<provider::ProviderResponse> doGenerate(
        const provider::ProviderRequest&,
        provider::ProviderCallContext&) override
    {
        ++calls_;
        if (mode_ == ProbeMode::Throw) {
            throw std::runtime_error("synthetic provider exception");
        }
        if (mode_ == ProbeMode::Failure) {
            return platform::Result<provider::ProviderResponse>::failure(
                platform::Error::providerError(
                    "synthetic provider failure", "UPSTREAM_SYNTHETIC", 503));
        }
        if (mode_ == ProbeMode::InvalidFailure) {
            // A concrete provider must never manufacture a failure without an
            // ErrorCode; ProviderBase owns the last-line normalization.
            return platform::Result<provider::ProviderResponse>::failure(
                platform::Error{});
        }

        provider::ProviderResponse response;
        response.text = "generated";
        return platform::Result<provider::ProviderResponse>::success(std::move(response));
    }

    std::string_view providerName() const noexcept override { return "probe"; }

  private:
    ProbeMode mode_;
    int calls_ = 0;
};

static_assert(std::is_base_of_v<provider::ProviderBase, ProbeProvider>,
              "production provider fixture must use ProviderBase");
static_assert(std::is_base_of_v<provider::IChatProvider, ProbeProvider>,
              "ProviderBase must remain an IChatProvider");

provider::ProviderCallContext makeContext(
    const platform::CancellationToken& token,
    std::chrono::milliseconds remaining = std::chrono::seconds(1))
{
    return provider::ProviderCallContext{
        token, std::chrono::steady_clock::now() + remaining};
}

}  // namespace

DROGON_TEST(ProviderBase_UsesNviForSuccessAndProductionFactory)
{
    platform::CancellationSource source;
    const auto token = source.token();
    auto context = makeContext(token);

    auto provider = provider::makeProductionProvider<ProbeProvider>(ProbeMode::Success);
    const auto result = provider->generate(provider::ProviderRequest{}, context);

    CHECK(result.ok());
    CHECK(result.value().text == "generated");
    CHECK(provider->capabilities().nativeToolCalls);
    CHECK(provider->capabilities().supportsImages);
}

DROGON_TEST(ProviderBase_RejectsCancelledAndExpiredCallsBeforeProviderWork)
{
    std::vector<platform::ErrorCode> observed;
    bool cancelledReporterNameWasProbe = false;
    ProbeProvider cancelled(
        ProbeMode::Success,
        [&observed, &cancelledReporterNameWasProbe](
            std::string_view name, const platform::Error& error) {
            cancelledReporterNameWasProbe = name == "probe";
            observed.push_back(error.code);
        });

    platform::CancellationSource cancelledSource;
    const auto cancelledToken = cancelledSource.token();
    cancelledSource.request();
    auto cancelledContext = makeContext(cancelledToken);
    const auto cancelledResult = cancelled.generate({}, cancelledContext);
    CHECK(!cancelledResult.ok());
    CHECK(cancelledResult.error().code == platform::ErrorCode::Cancelled);
    CHECK(cancelled.calls() == 0);
    CHECK(observed.size() == 1);
    CHECK(cancelledReporterNameWasProbe);

    ProbeProvider expired(
        ProbeMode::Success,
        [&observed](std::string_view, const platform::Error& error) {
            observed.push_back(error.code);
        });
    platform::CancellationSource activeSource;
    const auto activeToken = activeSource.token();
    auto expiredContext = makeContext(activeToken, std::chrono::milliseconds(-1));
    const auto expiredResult = expired.generate({}, expiredContext);
    CHECK(!expiredResult.ok());
    CHECK(expiredResult.error().code == platform::ErrorCode::Timeout);
    CHECK(expired.calls() == 0);
    CHECK(observed.size() == 2);
}

DROGON_TEST(ProviderBase_PreservesFailureAndReportsExactlyOnce)
{
    int reportCount = 0;
    bool reporterNameWasProbe = false;
    platform::Error reported;
    ProbeProvider provider(
        ProbeMode::Failure,
        [&reportCount, &reporterNameWasProbe, &reported](
            std::string_view name, const platform::Error& error) {
            reporterNameWasProbe = name == "probe";
            ++reportCount;
            reported = error;
        });

    platform::CancellationSource source;
    const auto token = source.token();
    auto context = makeContext(token);
    const auto result = provider.generate({}, context);

    CHECK(!result.ok());
    CHECK(provider.calls() == 1);
    CHECK(reportCount == 1);
    CHECK(reporterNameWasProbe);
    CHECK(result.error().code == platform::ErrorCode::ProviderError);
    CHECK(result.error().providerCode == "UPSTREAM_SYNTHETIC");
    CHECK(result.error().upstreamHttpStatus == 503);
    CHECK(reported.providerCode == result.error().providerCode);
}

DROGON_TEST(ProviderBase_NormalizesAnInvalidFailureBeforeReporting)
{
    int reportCount = 0;
    platform::Error reported;
    ProbeProvider provider(
        ProbeMode::InvalidFailure,
        [&reportCount, &reported](std::string_view, const platform::Error& error) {
            ++reportCount;
            reported = error;
        });

    platform::CancellationSource source;
    const auto token = source.token();
    auto context = makeContext(token);
    const auto result = provider.generate({}, context);

    CHECK(!result.ok());
    CHECK(provider.calls() == 1);
    CHECK(result.error().code == platform::ErrorCode::Internal);
    CHECK(result.error().message == "Provider returned an invalid failure result");
    CHECK(reportCount == 1);
    CHECK(reported.code == result.error().code);
}

DROGON_TEST(ProviderBase_ConvertsExceptionsWithoutLeakingThem)
{
    int reportCount = 0;
    ProbeProvider provider(
        ProbeMode::Throw,
        [&reportCount](std::string_view, const platform::Error&) { ++reportCount; });

    platform::CancellationSource source;
    const auto token = source.token();
    auto context = makeContext(token);
    const auto result = provider.generate({}, context);

    CHECK(!result.ok());
    CHECK(result.error().code == platform::ErrorCode::Internal);
    CHECK(result.error().message == "Provider execution failed");
    CHECK(result.error().detail == "synthetic provider exception");
    CHECK(reportCount == 1);
}
