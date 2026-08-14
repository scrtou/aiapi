#pragma once

#include <infrastructure/provider/ProviderBase.h>

#include <memory>
#include <type_traits>
#include <utility>

namespace provider {

/**
 * The only construction helper for a future production IChatProvider.
 *
 * The static assertion is intentionally in compiled C++ rather than merely a
 * source-text convention: a production provider cannot enter the P6 registry
 * without deriving from the NVI boundary.  Test fakes may still implement
 * IChatProvider directly.
 */
template <typename ProviderT, typename... Args>
[[nodiscard]] std::shared_ptr<IChatProvider> makeProductionProvider(Args&&... args)
{
    static_assert(std::is_base_of_v<ProviderBase, ProviderT>,
                  "production providers must derive from ProviderBase");
    static_assert(std::is_base_of_v<IChatProvider, ProviderT>,
                  "ProviderBase-derived production provider must implement IChatProvider");
    return std::make_shared<ProviderT>(std::forward<Args>(args)...);
}

}  // namespace provider
