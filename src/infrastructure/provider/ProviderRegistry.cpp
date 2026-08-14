#include <infrastructure/provider/ProviderRegistry.h>

#include <algorithm>
#include <utility>

namespace provider {

bool ProviderRegistry::registerProvider(
    std::string name,
    std::shared_ptr<APIinterface> provider)
{
    if (frozen_ || name.empty() || !provider) return false;
    return providers_.emplace(std::move(name), std::move(provider)).second;
}

std::shared_ptr<APIinterface> ProviderRegistry::findProvider(
    const std::string& apiName) const
{
    const auto it = providers_.find(apiName);
    return it == providers_.end() ? nullptr : it->second;
}

std::vector<std::string> ProviderRegistry::providerNames() const
{
    std::vector<std::string> names;
    names.reserve(providers_.size());
    for (const auto& entry : providers_) names.push_back(entry.first);
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace provider
