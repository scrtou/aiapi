#include <drogon/drogon_test.h>

#include <platform/result/Result.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

template <typename Callable>
bool throwsLogicError(Callable&& callable)
{
    try {
        callable();
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

}  // namespace

DROGON_TEST(Result_SuccessAndFailureKeepTheirDistinctPayloads)
{
    auto success = platform::Result<std::string>::success("ready");
    CHECK(success.ok());
    CHECK(success.value() == "ready");

    auto failure = platform::Result<std::string>::failure(
        platform::Error::providerError("upstream refused", "E_UPSTREAM", 503));
    CHECK(!failure.ok());
    CHECK(failure.error().code == platform::ErrorCode::ProviderError);
    CHECK(failure.error().providerCode == "E_UPSTREAM");
    CHECK(failure.error().upstreamHttpStatus == 503);
    CHECK(failure.error().httpStatus() == 502);
}

DROGON_TEST(Result_SupportsMoveOnlyAndErrorAsItsSuccessValue)
{
    auto moveOnly = platform::Result<std::unique_ptr<int>>::success(
        std::make_unique<int>(42));
    CHECK(moveOnly.ok());
    auto value = std::move(moveOnly).value();
    CHECK(value != nullptr);
    CHECK(*value == 42);

    const auto errorAsValue = platform::Error::timeout("value, not failure");
    auto errorValue = platform::Result<platform::Error>::success(errorAsValue);
    CHECK(errorValue.ok());
    CHECK(errorValue.value().code == platform::ErrorCode::Timeout);
    CHECK(errorValue.value().message == "value, not failure");
}

DROGON_TEST(Result_ValueAndErrorAccessFailLoudlyOnTheWrongState)
{
    auto failure = platform::Result<int>::failure(platform::Error::internal("failed"));
    CHECK(throwsLogicError([&failure] { (void)failure.value(); }));

    auto success = platform::Result<int>::success(7);
    CHECK(throwsLogicError([&success] { (void)success.error(); }));
}

DROGON_TEST(Result_VoidSpecializationHasTheSameExplicitSemantics)
{
    auto success = platform::Result<void>::success();
    CHECK(success.ok());
    success.value();

    auto failure = platform::Result<void>::failure(platform::Error::cancelled("cancelled"));
    CHECK(!failure.ok());
    CHECK(failure.error().code == platform::ErrorCode::Cancelled);
    CHECK(throwsLogicError([&failure] { failure.value(); }));
}

DROGON_TEST(Result_ErrorCodeMappingIsStable)
{
    CHECK(platform::defaultHttpStatus(platform::ErrorCode::BadRequest) == 400);
    CHECK(platform::defaultHttpStatus(platform::ErrorCode::ProviderError) == 502);
    CHECK(platform::errorCodeName(platform::ErrorCode::Cancelled) == "cancelled");
}
