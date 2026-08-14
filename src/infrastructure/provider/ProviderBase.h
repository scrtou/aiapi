#pragma once

#include <domain/port/IChatProvider.h>

#include <functional>
#include <string_view>

namespace provider {

/**
 * Thin NVI boundary shared by production providers.
 *
 * It intentionally does not prescribe HTTP, retry, polling, account
 * selection, or event protocol mechanics.  Concrete providers own those
 * workflows through composition; the base only makes cancellation, deadline,
 * exception conversion, result sanity, and one-shot error reporting uniform.
 */
class ProviderBase : public IChatProvider
{
  public:
    using FailureObserver = std::function<void(std::string_view, const platform::Error&)>;

    explicit ProviderBase(FailureObserver failureObserver = {});
    ~ProviderBase() override = default;

    platform::Result<ProviderResponse> generate(
        const ProviderRequest& request,
        ProviderCallContext& context) final;

  protected:
    virtual platform::Result<ProviderResponse> doGenerate(
        const ProviderRequest& request,
        ProviderCallContext& context) = 0;

    virtual std::string_view providerName() const noexcept = 0;

  private:
    platform::Result<ProviderResponse> fail(platform::Error error) const;
    void reportFailure(const platform::Error& error) const noexcept;

    FailureObserver failureObserver_;
};

}  // namespace provider
