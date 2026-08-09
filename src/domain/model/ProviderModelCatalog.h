#ifndef PROVIDER_MODEL_CATALOG_H
#define PROVIDER_MODEL_CATALOG_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct ProviderModelCapabilities
{
    bool images = false;
    bool imagesDeclared = false;
    bool functionCalling = false;
    bool googleSearch = false;
    bool thinking = false;
    std::vector<std::string> supportedMimeTypes;
};

struct ChaynsModelExtension
{
    std::string personId;
    std::optional<int> usedModel;
    std::optional<std::int64_t> tobitId;
    bool requiresSidekickPro = false;
    ProviderModelCapabilities capabilities;
    std::map<std::string, int> skills;
    std::string knowledge;
    std::string developerName;
    std::string developerCountry;
    std::string hostingProvider;
    std::string hostingCountry;
    bool hostingInEurope = false;
    std::optional<int> costIndicator;
};

struct ProviderModel
{
    std::string id;
    std::optional<std::int64_t> created;
    std::optional<std::string> ownedBy;
    std::optional<ChaynsModelExtension> chayns;
};

struct ProviderModelCatalog
{
    std::vector<ProviderModel> models;
};

#endif  // PROVIDER_MODEL_CATALOG_H
