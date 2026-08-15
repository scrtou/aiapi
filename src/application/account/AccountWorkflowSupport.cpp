#include <application/account/AccountWorkflowSupport.h>

#include <domain/port/IKeyValueConfigStore.h>
#include <platform/LocalDateTime.h>

#include <cstdlib>
#include <sstream>
#include <utility>

namespace account::workflow {
namespace {

constexpr const char* kAutoDeleteEnabledKey = "account_automation.auto_delete_enabled";
constexpr const char* kDeleteAfterDaysKey = "account_automation.delete_after_days";
constexpr const char* kAutoRegisterEnabledKey = "account_automation.auto_register_enabled";
constexpr const char* kNamespaceToolBridgeEnabledKey = "tool_bridge.namespace_enabled";
constexpr int kDefaultDeleteAfterDays = 6;

std::string boolToConfigValue(bool value)
{
    return value ? "true" : "false";
}

}  // namespace

AccountAutomationSettings loadAutomationSettingsFromCustomConfig(const Json::Value& customConfig)
{
    AccountAutomationSettings settings;
    if (customConfig.isMember("account_automation") && customConfig["account_automation"].isObject()) {
        const auto& automation = customConfig["account_automation"];
        settings.autoDeleteEnabled = automation.get("auto_delete_enabled", settings.autoDeleteEnabled).asBool();
        settings.deleteAfterDays = automation.get("delete_after_days", settings.deleteAfterDays).asInt();
        settings.autoRegisterEnabled = automation.get("auto_register_enabled", settings.autoRegisterEnabled).asBool();
    }
    if (customConfig.isMember("tool_bridge") && customConfig["tool_bridge"].isObject()) {
        const auto& toolBridge = customConfig["tool_bridge"];
        settings.namespaceToolBridgeEnabled = toolBridge.get(
            "namespace_enabled", settings.namespaceToolBridgeEnabled).asBool();
    }
    if (settings.deleteAfterDays <= 0) {
        settings.deleteAfterDays = kDefaultDeleteAfterDays;
    }
    return settings;
}

bool saveAutomationSettings(IKeyValueConfigStore& store,
                            const AccountAutomationSettings& settings,
                            std::string* errorMessage)
{
    return store.setValues({
        {kAutoDeleteEnabledKey, boolToConfigValue(settings.autoDeleteEnabled)},
        {kDeleteAfterDaysKey, std::to_string(settings.deleteAfterDays)},
        {kAutoRegisterEnabledKey, boolToConfigValue(settings.autoRegisterEnabled)},
        {kNamespaceToolBridgeEnabledKey, boolToConfigValue(settings.namespaceToolBridgeEnabled)},
    }, errorMessage);
}

bool shouldSkipLifecycleRefresh(const std::shared_ptr<Accountinfo_st>& account)
{
    return account && (account->status == AccountStatus::WAITING ||
                       account->status == AccountStatus::REGISTERING);
}

bool shouldExcludeFromPool(const std::shared_ptr<Accountinfo_st>& account)
{
    return shouldSkipLifecycleRefresh(account);
}

bool isRetoolWorkspaceActive(const RetoolWorkspaceInfo& workspace)
{
    return workspace.status != "disabled";
}

bool splitUrl(const std::string& fullUrl, std::string& baseUrl, std::string& path)
{
    const size_t protocolPos = fullUrl.find("://");
    if (protocolPos == std::string::npos) {
        return false;
    }
    const size_t pathPos = fullUrl.find('/', protocolPos + 3);
    if (pathPos == std::string::npos) {
        baseUrl = fullUrl;
        path = "/";
    } else {
        baseUrl = fullUrl.substr(0, pathPos);
        path = fullUrl.substr(pathPos);
    }
    return true;
}

bool parseJsonBody(const std::string& body, Json::Value& out, std::string& errors)
{
    Json::CharReaderBuilder reader;
    std::istringstream stream(body);
    return Json::parseFromStream(reader, stream, &out, &errors);
}

bool isSuccessEnvelope(const Json::Value& json)
{
    return json.isObject() && json.isMember("success") && json["success"].asBool() &&
           json.isMember("data");
}

std::string extractErrorMessageFromEnvelope(const Json::Value& json,
                                            const std::string& fallback)
{
    if (json.isObject() && json.isMember("error") && json["error"].isObject()) {
        return json["error"].get("message", fallback).asString();
    }
    return fallback;
}

std::string currentLocalDbTimestamp()
{
    return platform::localDbTimestampNow();
}

std::string loginServiceUrl(const Json::Value& customConfig, const std::string& provider)
{
    if (customConfig.isMember("login_service_urls") && customConfig["login_service_urls"].isArray()) {
        for (const auto& service : customConfig["login_service_urls"]) {
            if (service.isMember("name") && service["name"].asString() == provider &&
                service.isMember("url")) {
                const std::string url = service["url"].asString();
                if (!url.empty()) {
                    return url;
                }
            }
        }
    }

    const char* envUrl = std::getenv("LOGIN_SERVICE_URL");
    if (envUrl != nullptr && *envUrl != '\0' && provider == "chaynsapi") {
        return envUrl;
    }
    return provider == "chaynsapi" ? "http://127.0.0.1:8004/api/v1/logins" : "";
}

std::string registrationServiceUrl(const Json::Value& customConfig, const std::string& provider)
{
    if (customConfig.isMember("regist_service_urls") && customConfig["regist_service_urls"].isArray()) {
        for (const auto& service : customConfig["regist_service_urls"]) {
            if (service.isMember("name") && service["name"].asString() == provider &&
                service.isMember("url")) {
                const std::string url = service["url"].asString();
                if (!url.empty()) {
                    return url;
                }
            }
        }
    }

    const char* envUrl = std::getenv("REGIST_SERVICE_URL");
    if (envUrl != nullptr && *envUrl != '\0' && provider == "chaynsapi") {
        return envUrl;
    }
    return provider == "chaynsapi"
        ? "http://127.0.0.1:8000/api/v1/workflows/register-and-login"
        : "";
}

std::string downstreamBearerApiKey(const Json::Value& customConfig,
                                   const std::string& provider)
{
    if (customConfig.isMember("downstream_service_api_keys") &&
        customConfig["downstream_service_api_keys"].isArray()) {
        for (const auto& service : customConfig["downstream_service_api_keys"]) {
            if (service.isMember("name") && service["name"].asString() == provider &&
                service.isMember("api_key")) {
                const std::string key = service["api_key"].asString();
                if (!key.empty()) {
                    return key;
                }
            }
        }
    }

    const char* configured = std::getenv("DOWNSTREAM_SERVICE_API_KEY");
    if (configured != nullptr && *configured != '\0') {
        return configured;
    }
    const char* legacy = std::getenv("AIAPI_TOOL_BEARER_KEY");
    if (legacy != nullptr && *legacy != '\0') {
        return legacy;
    }
    return "";
}

}  // namespace account::workflow
