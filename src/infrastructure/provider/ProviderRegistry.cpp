#include <infrastructure/provider/ProviderRegistry.h>

#include <algorithm>
#include <utility>

namespace provider {

bool ProviderRegistry::nameIsRegistered(const std::string& name) const
{
    return legacyProviders_.find(name) != legacyProviders_.end() ||
           chatProviders_.find(name) != chatProviders_.end();
}

bool ProviderRegistry::registerProvider(
    std::string name,
    std::shared_ptr<APIinterface> provider)
{
    if (frozen_ || name.empty() || !provider || nameIsRegistered(name)) return false;
    return legacyProviders_.emplace(std::move(name), std::move(provider)).second;
}

bool ProviderRegistry::registerChatProvider(
    std::string name,
    std::shared_ptr<IChatProvider> provider,
    std::shared_ptr<IProviderModelCatalog> catalog,
    std::shared_ptr<IProviderThreadContext> threadContext)
{
    if (frozen_ || name.empty() || !provider || nameIsRegistered(name)) return false;

    const auto inserted = chatProviders_.emplace(name, std::move(provider)).second;
    if (!inserted) return false;
    if (catalog) catalogs_.emplace(name, std::move(catalog));
    if (threadContext) threadContexts_.emplace(std::move(name), std::move(threadContext));
    return true;
}

std::shared_ptr<APIinterface> ProviderRegistry::findProvider(
    const std::string& apiName) const
{
    const auto it = legacyProviders_.find(apiName);
    return it == legacyProviders_.end() ? nullptr : it->second;
}

std::shared_ptr<IChatProvider> ProviderRegistry::findChatProvider(
    const std::string& apiName) const
{
    const auto it = chatProviders_.find(apiName);
    return it == chatProviders_.end() ? nullptr : it->second;
}

std::shared_ptr<IProviderModelCatalog> ProviderRegistry::findModelCatalog(
    const std::string& apiName) const
{
    const auto it = catalogs_.find(apiName);
    return it == catalogs_.end() ? nullptr : it->second;
}

std::shared_ptr<IProviderThreadContext> ProviderRegistry::findThreadContext(
    const std::string& apiName) const
{
    const auto it = threadContexts_.find(apiName);
    return it == threadContexts_.end() ? nullptr : it->second;
}

std::vector<std::string> ProviderRegistry::providerNames() const
{
    std::vector<std::string> names;
    names.reserve(legacyProviders_.size() + chatProviders_.size());
    for (const auto& entry : legacyProviders_) names.push_back(entry.first);
    for (const auto& entry : chatProviders_) names.push_back(entry.first);
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace provider
