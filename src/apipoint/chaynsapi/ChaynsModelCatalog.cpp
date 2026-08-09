#include <apipoint/chaynsapi/ChaynsModelCatalog.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>

namespace chayns {
namespace {

std::string trimCopy(const std::string& value)
{
    auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    return begin < end ? std::string(begin, end) : std::string();
}

std::string readString(const Json::Value& source, const char* key)
{
    const auto& value = source[key];
    return value.isString() ? trimCopy(value.asString()) : std::string();
}

bool readOptionalBool(const Json::Value& source,
                      const char* key,
                      bool malformedDefault,
                      std::vector<std::string>& warnings,
                      const std::string& modelId)
{
    if (!source.isMember(key)) {
        return false;
    }
    if (source[key].isBool()) {
        return source[key].asBool();
    }
    warnings.push_back("模型 " + modelId + " 的字段 " + key + " 不是布尔值");
    return malformedDefault;
}

bool readInteger(const Json::Value& source,
                 const char* key,
                 Json::Int64& output,
                 std::vector<std::string>& warnings,
                 const std::string& modelId)
{
    if (!source.isMember(key)) {
        return false;
    }
    const auto& value = source[key];
    if (!value.isIntegral()) {
        warnings.push_back("模型 " + modelId + " 的字段 " + key + " 不是整数");
        return false;
    }
    output = value.asInt64();
    return true;
}

Json::Value buildOpenAiModel(const ModelDescriptor& descriptor)
{
    Json::Value output(Json::objectValue);
    output["id"] = descriptor.id;
    output["object"] = "model";
    // The upstream payload does not provide a model creation timestamp. Do not
    // misrepresent the knowledge cutoff or catalog fetch time as creation time.
    output["created"] = Json::Int64(0);
    output["owned_by"] = descriptor.developer.empty() ? "chayns" : descriptor.developer;

    Json::Value extension(Json::objectValue);
    extension["person_id"] = descriptor.personId;
    if (descriptor.usedModel > 0) {
        extension["used_model"] = descriptor.usedModel;
    }
    if (descriptor.tobitId > 0) {
        extension["tobit_id"] = Json::Int64(descriptor.tobitId);
    }
    extension["requires_sidekick_pro"] = descriptor.requiresPro;

    Json::Value capabilities(Json::objectValue);
    capabilities["images"] = supportsImageInput(descriptor);
    capabilities["images_declared"] = descriptor.canHandleImages;
    capabilities["function_calling"] = descriptor.canHandleFunctionCalling;
    capabilities["google_search"] = descriptor.canHandleGoogleSearch;
    capabilities["thinking"] = descriptor.canUseThinking;
    Json::Value mimeTypes(Json::arrayValue);
    for (const auto& mimeType : descriptor.supportedMimeTypes) {
        mimeTypes.append(mimeType);
    }
    capabilities["supported_mime_types"] = std::move(mimeTypes);
    extension["capabilities"] = std::move(capabilities);

    Json::Value skills(Json::objectValue);
    for (const auto& item : descriptor.skills) {
        skills[item.first] = item.second;
    }
    extension["skills"] = std::move(skills);

    if (!descriptor.knowledge.empty()) {
        extension["knowledge"] = descriptor.knowledge;
    }

    Json::Value developer(Json::objectValue);
    if (!descriptor.developer.empty()) {
        developer["name"] = descriptor.developer;
    }
    if (!descriptor.developerCountry.empty()) {
        developer["country"] = descriptor.developerCountry;
    }
    extension["developer"] = std::move(developer);

    Json::Value hosting(Json::objectValue);
    if (!descriptor.hostingProvider.empty()) {
        hosting["provider"] = descriptor.hostingProvider;
    }
    if (!descriptor.hostingCountry.empty()) {
        hosting["country"] = descriptor.hostingCountry;
    }
    hosting["in_europe"] = descriptor.hostingInEurope;
    extension["hosting"] = std::move(hosting);

    if (descriptor.costIndicator >= 0) {
        extension["cost_indicator"] = descriptor.costIndicator;
    }

    output["x_chayns"] = std::move(extension);
    return output;
}

}  // namespace

std::string normalizeMimeType(const std::string& mimeType)
{
    std::string normalized = trimCopy(mimeType);
    const auto parameter = normalized.find(';');
    if (parameter != std::string::npos) {
        normalized.erase(parameter);
    }
    normalized = trimCopy(normalized);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return normalized;
}

bool mimeTypeMatches(const std::string& supported, const std::string& requested)
{
    const std::string normalizedSupported = normalizeMimeType(supported);
    const std::string normalizedRequested = normalizeMimeType(requested);
    if (normalizedSupported.empty() || normalizedRequested.empty()) {
        return false;
    }
    if (normalizedSupported == "*/*" || normalizedSupported == normalizedRequested) {
        return true;
    }
    const auto slash = normalizedSupported.find('/');
    return slash != std::string::npos &&
           normalizedSupported.substr(slash + 1) == "*" &&
           normalizedRequested.compare(0, slash + 1, normalizedSupported, 0, slash + 1) == 0;
}

bool supportsMimeType(const ModelDescriptor& model, const std::string& requested)
{
    const std::string normalizedRequested = normalizeMimeType(requested);
    if (normalizedRequested.empty()) {
        return true;  // Remote image URLs may not carry a MIME type in the request.
    }
    for (const auto& supported : model.supportedMimeTypes) {
        if (mimeTypeMatches(supported, normalizedRequested)) {
            return true;
        }
    }

    if (!model.canHandleImages || normalizedRequested.rfind("image/", 0) != 0) {
        return false;
    }

    // Upstream metadata sometimes sets canHandleImages=true but publishes a
    // text-only supportedMimeTypes list. In that exact conflict, trust the
    // explicit capability flag for common image inputs supported by the
    // uploader. Once at least one image MIME
    // is advertised, however, the list is treated as an intentional allowlist
    // (for example image/png must not silently permit image/jpeg).
    const bool declaresAnyImageMime = std::any_of(
        model.supportedMimeTypes.begin(),
        model.supportedMimeTypes.end(),
        [](const std::string& mimeType) {
            const std::string normalized = normalizeMimeType(mimeType);
            return normalized == "image/*" || normalized.rfind("image/", 0) == 0;
        });
    const bool isCommonImageMime =
        normalizedRequested == "image/png" ||
        normalizedRequested == "image/jpeg" ||
        normalizedRequested == "image/jpg" ||
        normalizedRequested == "image/webp" ||
        normalizedRequested == "image/gif";
    return !declaresAnyImageMime && isCommonImageMime;
}

bool supportsImageInput(const ModelDescriptor& model)
{
    if (model.canHandleImages) {
        return true;
    }
    return std::any_of(model.supportedMimeTypes.begin(), model.supportedMimeTypes.end(), [](const std::string& mimeType) {
        const std::string normalized = normalizeMimeType(mimeType);
        return normalized == "image/*" || normalized.rfind("image/", 0) == 0;
    });
}

ParseResult parseModelCatalog(const Json::Value& payload)
{
    ParseResult result;
    result.catalog.openAiResponse["object"] = "list";
    result.catalog.openAiResponse["data"] = Json::Value(Json::arrayValue);

    if (!payload.isArray()) {
        result.warnings.push_back("模型列表顶层不是数组");
        return result;
    }

    std::set<std::string> seenMimeTypes;
    for (Json::ArrayIndex index = 0; index < payload.size(); ++index) {
        const auto& source = payload[index];
        if (!source.isObject()) {
            ++result.skipped;
            result.warnings.push_back("模型列表第 " + std::to_string(index) + " 项不是对象");
            continue;
        }

        ModelDescriptor descriptor;
        descriptor.id = readString(source, "showName");
        descriptor.personId = readString(source, "personId");
        if (descriptor.id.empty() || descriptor.personId.empty()) {
            ++result.skipped;
            result.warnings.push_back("模型列表第 " + std::to_string(index) + " 项缺少 showName 或 personId");
            continue;
        }
        if (result.catalog.byName.find(descriptor.id) != result.catalog.byName.end()) {
            ++result.duplicates;
            result.warnings.push_back("模型名称重复，已忽略后续项: " + descriptor.id);
            continue;
        }

        descriptor.raw = source;

        Json::Int64 integerValue = 0;
        if (readInteger(source, "usedModel", integerValue, result.warnings, descriptor.id) &&
            integerValue > 0 && integerValue <= std::numeric_limits<int>::max()) {
            descriptor.usedModel = static_cast<int>(integerValue);
        }
        if (readInteger(source, "tobitId", integerValue, result.warnings, descriptor.id) && integerValue > 0) {
            descriptor.tobitId = integerValue;
        }

        // The upstream convention omits needSidekickPro for free models. A
        // malformed present value is handled fail-closed and requires Pro.
        descriptor.requiresPro = readOptionalBool(
            source, "needSidekickPro", true, result.warnings, descriptor.id);
        descriptor.canHandleImages = readOptionalBool(
            source, "canHandleImages", false, result.warnings, descriptor.id);
        descriptor.canHandleFunctionCalling = readOptionalBool(
            source, "canHandleFunctionCalling", false, result.warnings, descriptor.id);
        descriptor.canHandleGoogleSearch = readOptionalBool(
            source, "canHandleGoogleSearch", false, result.warnings, descriptor.id);
        descriptor.canUseThinking = readOptionalBool(
            source, "canUseThinking", false, result.warnings, descriptor.id);

        if (source.isMember("supportedMimeTypes")) {
            if (source["supportedMimeTypes"].isArray()) {
                seenMimeTypes.clear();
                for (const auto& item : source["supportedMimeTypes"]) {
                    if (!item.isString()) {
                        result.warnings.push_back("模型 " + descriptor.id + " 包含非字符串 MIME 类型");
                        continue;
                    }
                    const std::string mimeType = normalizeMimeType(item.asString());
                    if (!mimeType.empty() && seenMimeTypes.insert(mimeType).second) {
                        descriptor.supportedMimeTypes.push_back(mimeType);
                    }
                }
            } else {
                result.warnings.push_back("模型 " + descriptor.id + " 的 supportedMimeTypes 不是数组");
            }
        }

        const bool declaresImageMime = std::any_of(
            descriptor.supportedMimeTypes.begin(),
            descriptor.supportedMimeTypes.end(),
            [](const std::string& mimeType) {
                return mimeType == "image/*" || mimeType.rfind("image/", 0) == 0;
            });
        if (descriptor.canHandleImages && !descriptor.supportedMimeTypes.empty() && !declaresImageMime) {
            result.imageCapabilityConflictModels.push_back(descriptor.id);
        } else if (!descriptor.canHandleImages && declaresImageMime) {
            result.warnings.push_back(
                "模型 " + descriptor.id + " 包含图片 MIME 类型，但未声明 canHandleImages");
        }

        if (source.isMember("skills")) {
            if (source["skills"].isObject()) {
                for (const auto& skillName : source["skills"].getMemberNames()) {
                    const auto& score = source["skills"][skillName];
                    if (!score.isIntegral()) {
                        result.warnings.push_back("模型 " + descriptor.id + " 的技能 " + skillName + " 不是整数");
                        continue;
                    }
                    const int value = score.asInt();
                    if (value < 0 || value > 100) {
                        result.warnings.push_back("模型 " + descriptor.id + " 的技能 " + skillName + " 超出 0-100");
                        continue;
                    }
                    descriptor.skills[skillName] = value;
                }
            } else {
                result.warnings.push_back("模型 " + descriptor.id + " 的 skills 不是对象");
            }
        }

        descriptor.knowledge = readString(source, "knowledge");
        descriptor.developer = readString(source, "developer");
        descriptor.developerCountry = readString(source, "developerCountry");
        descriptor.hostingProvider = readString(source, "hostingProvider");
        descriptor.hostingCountry = readString(source, "hostingCountry");
        descriptor.hostingInEurope = readOptionalBool(
            source, "hostingInEurope", false, result.warnings, descriptor.id);

        if (readInteger(source, "costIndicator", integerValue, result.warnings, descriptor.id)) {
            if (integerValue >= 0 && integerValue <= 100) {
                descriptor.costIndicator = static_cast<int>(integerValue);
            } else {
                result.warnings.push_back("模型 " + descriptor.id + " 的 costIndicator 超出 0-100");
            }
        }

        if (!result.catalog.nameByPersonId.emplace(descriptor.personId, descriptor.id).second) {
            result.warnings.push_back("多个模型共享 personId: " + descriptor.personId);
        }

        result.catalog.openAiResponse["data"].append(buildOpenAiModel(descriptor));
        result.catalog.byName.emplace(descriptor.id, std::move(descriptor));
    }

    result.valid = true;
    return result;
}

}  // namespace chayns
