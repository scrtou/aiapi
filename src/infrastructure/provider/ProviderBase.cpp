#include <infrastructure/provider/ProviderBase.h>

#include <exception>
#include <utility>

namespace provider {

ProviderBase::ProviderBase(FailureObserver failureObserver)
    : failureObserver_(std::move(failureObserver))
{
}

platform::Result<ProviderResponse> ProviderBase::generate(
    const ProviderRequest& request,
    ProviderCallContext& context)
{
    if (context.isCancelled()) {
        return fail(platform::Error::cancelled("Provider request cancelled"));
    }
    if (context.deadlineExceeded()) {
        return fail(platform::Error::timeout("Provider request deadline exceeded"));
    }

    try {
        auto result = doGenerate(request, context);
        if (!result) {
            auto error = result.error();
            if (!error.hasError()) {
                error = platform::Error::internal(
                    "Provider returned an invalid failure result");
            }
            reportFailure(error);
            return platform::Result<ProviderResponse>::failure(std::move(error));
        }
        return result;
    } catch (const std::exception& exception) {
        return fail(platform::Error::internal(
            "Provider execution failed", exception.what()));
    } catch (...) {
        return fail(platform::Error::internal("Provider execution failed"));
    }
}

platform::Result<ProviderResponse> ProviderBase::fail(platform::Error error) const
{
    reportFailure(error);
    return platform::Result<ProviderResponse>::failure(std::move(error));
}

void ProviderBase::reportFailure(const platform::Error& error) const noexcept
{
    if (!failureObserver_) return;
    try {
        const auto name = providerName();
        failureObserver_(name.empty() ? std::string_view{"unknown"} : name, error);
    } catch (...) {
        // Error reporting must not replace the original provider failure.
    }
}

}  // namespace provider
