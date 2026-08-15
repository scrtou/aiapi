#ifndef PROVIDER_MODEL_CATALOG_JSON_CODEC_H
#define PROVIDER_MODEL_CATALOG_JSON_CODEC_H

#include <domain/model/ProviderModelCatalog.h>
#include <json/json.h>

namespace providermodelcodec {

inline Json::Value toJson(const ProviderModelCatalog& catalog)
{
    Json::Value root(Json::objectValue);
    root["object"] = "list";
    root["data"] = Json::Value(Json::arrayValue);
    for (const auto& model : catalog.models)
    {
        Json::Value item(Json::objectValue);
        item["id"] = model.id;
        item["object"] = "model";
        if (model.created) item["created"] = Json::Int64(*model.created);
        if (model.ownedBy) item["owned_by"] = *model.ownedBy;
        if (model.chayns)
        {
            const auto& source = *model.chayns;
            Json::Value extension(Json::objectValue);
            extension["person_id"] = source.personId;
            if (source.usedModel) extension["used_model"] = *source.usedModel;
            if (source.tobitId) extension["tobit_id"] = Json::Int64(*source.tobitId);
            extension["requires_sidekick_pro"] = source.requiresSidekickPro;

            Json::Value capabilities(Json::objectValue);
            capabilities["images"] = source.capabilities.images;
            capabilities["images_declared"] = source.capabilities.imagesDeclared;
            capabilities["function_calling"] = source.capabilities.functionCalling;
            capabilities["google_search"] = source.capabilities.googleSearch;
            capabilities["thinking"] = source.capabilities.thinking;
            capabilities["supported_mime_types"] = Json::Value(Json::arrayValue);
            for (const auto& mime : source.capabilities.supportedMimeTypes)
                capabilities["supported_mime_types"].append(mime);
            extension["capabilities"] = std::move(capabilities);

            extension["skills"] = Json::Value(Json::objectValue);
            for (const auto& skill : source.skills)
                extension["skills"][skill.first] = skill.second;
            if (!source.knowledge.empty()) extension["knowledge"] = source.knowledge;

            extension["developer"] = Json::Value(Json::objectValue);
            if (!source.developerName.empty())
                extension["developer"]["name"] = source.developerName;
            if (!source.developerCountry.empty())
                extension["developer"]["country"] = source.developerCountry;

            extension["hosting"] = Json::Value(Json::objectValue);
            if (!source.hostingProvider.empty())
                extension["hosting"]["provider"] = source.hostingProvider;
            if (!source.hostingCountry.empty())
                extension["hosting"]["country"] = source.hostingCountry;
            extension["hosting"]["in_europe"] = source.hostingInEurope;
            if (source.costIndicator)
                extension["cost_indicator"] = *source.costIndicator;
            item["x_chayns"] = std::move(extension);
        }
        root["data"].append(std::move(item));
    }
    return root;
}

}  // namespace providermodelcodec

#endif  // PROVIDER_MODEL_CATALOG_JSON_CODEC_H
