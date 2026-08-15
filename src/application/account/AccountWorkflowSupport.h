#pragma once

#include <domain/model/AccountData.h>
#include <domain/model/RetoolWorkspaceInfo.h>

#include <json/json.h>

#include <string>

class IKeyValueConfigStore;

namespace account::workflow {

AccountAutomationSettings loadAutomationSettingsFromCustomConfig(const Json::Value& customConfig);
bool saveAutomationSettings(IKeyValueConfigStore& store,
                            const AccountAutomationSettings& settings,
                            std::string* errorMessage);

bool shouldSkipLifecycleRefresh(const std::shared_ptr<Accountinfo_st>& account);
bool shouldExcludeFromPool(const std::shared_ptr<Accountinfo_st>& account);
bool isRetoolWorkspaceActive(const RetoolWorkspaceInfo& workspace);

bool splitUrl(const std::string& fullUrl, std::string& baseUrl, std::string& path);
bool parseJsonBody(const std::string& body, Json::Value& out, std::string& errors);
bool isSuccessEnvelope(const Json::Value& json);
std::string extractErrorMessageFromEnvelope(const Json::Value& json,
                                            const std::string& fallback);

std::string currentLocalDbTimestamp();

std::string loginServiceUrl(const Json::Value& runtimeConfig, const std::string& provider);
std::string registrationServiceUrl(const Json::Value& runtimeConfig, const std::string& provider);
std::string downstreamBearerApiKey(const Json::Value& runtimeConfig,
                                  const std::string& provider);

}  // namespace account::workflow
