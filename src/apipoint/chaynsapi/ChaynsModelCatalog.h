#ifndef CHAYNS_MODEL_CATALOG_H
#define CHAYNS_MODEL_CATALOG_H

#include <json/json.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace chayns {

struct ModelDescriptor {
    std::string id;
    std::string personId;
    int usedModel = 0;
    std::int64_t tobitId = 0;

    bool requiresPro = false;
    bool canHandleImages = false;
    bool canHandleFunctionCalling = false;
    bool canHandleGoogleSearch = false;
    bool canUseThinking = false;

    std::vector<std::string> supportedMimeTypes;
    std::map<std::string, int> skills;

    std::string knowledge;
    std::string developer;
    std::string developerCountry;
    std::string hostingProvider;
    std::string hostingCountry;
    bool hostingInEurope = false;
    int costIndicator = -1;

    // Preserve the source object so newly-added upstream fields are not lost.
    Json::Value raw{Json::objectValue};
};

struct ModelCatalog {
    std::map<std::string, ModelDescriptor> byName;
    std::map<std::string, std::string> nameByPersonId;
    Json::Value openAiResponse{Json::objectValue};
};

struct ParseResult {
    bool valid = false;
    ModelCatalog catalog;
    std::vector<std::string> warnings;
    // Models that explicitly claim image support while publishing a nonempty
    // MIME list without any image MIME. Kept separate so callers can emit one
    // actionable aggregate warning instead of one warning per model.
    std::vector<std::string> imageCapabilityConflictModels;
    std::size_t skipped = 0;
    std::size_t duplicates = 0;
};

ParseResult parseModelCatalog(const Json::Value& payload);

std::string normalizeMimeType(const std::string& mimeType);
bool mimeTypeMatches(const std::string& supported, const std::string& requested);
bool supportsMimeType(const ModelDescriptor& model, const std::string& requested);
bool supportsImageInput(const ModelDescriptor& model);

}  // namespace chayns

#endif
