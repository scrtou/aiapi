#pragma once

#include <domain/model/ProviderCallContext.h>
#include <domain/model/ProviderCapabilities.h>
#include <domain/model/ProviderRequest.h>
#include <domain/model/ProviderResponse.h>
#include <platform/result/Result.h>

namespace provider {

/** Narrow provider port used by the P6 vertical slices. */
class IChatProvider
{
  public:
    virtual ~IChatProvider() = default;

    virtual platform::Result<ProviderResponse> generate(
        const ProviderRequest& request,
        ProviderCallContext& context) = 0;

    virtual ProviderCapabilities capabilities() const noexcept = 0;
};

}  // namespace provider
