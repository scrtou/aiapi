#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <json/json.h>

// Keep the mail policy used to create Nexos accounts in one small, testable
// unit.  GPTMail currently does not honour a requested Gmail domain, so it
// must not be a silent fallback for the default Nexos workflow.  Operators can
// still opt into an alternate provider/domain pair in custom_config.
namespace nexos {

inline constexpr int kDefaultRegistrationMailExpiryTimeMs = 60 * 60 * 1000;

struct RegistrationMailPolicy {
    std::vector<std::string> providers{"smailpro_web"};
    std::vector<std::string> domainPreference{"gmail.com"};
    int expiryTimeMs = kDefaultRegistrationMailExpiryTimeMs;
};

inline std::string trimRegistrationMailPolicyValue(const std::string& input)
{
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

inline bool isSafeRegistrationMailProviderName(const std::string& value)
{
    if (value.empty()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-';
    });
}

inline bool isSafeRegistrationMailDomain(const std::string& value)
{
    if (value.empty() || value.front() == '.' || value.back() == '.' ||
        value.front() == '-' || value.back() == '-' ||
        value.find('.') == std::string::npos) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '.' || ch == '-';
    });
}

inline std::string lowercaseRegistrationMailPolicyValue(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline bool parseRegistrationMailStringList(
    const Json::Value& value,
    bool isDomain,
    std::vector<std::string>& output)
{
    if (!value.isArray() || value.empty()) {
        return false;
    }

    std::vector<std::string> parsed;
    std::unordered_set<std::string> seen;
    parsed.reserve(value.size());
    for (const auto& item : value) {
        if (!item.isString()) {
            return false;
        }

        std::string itemValue = trimRegistrationMailPolicyValue(item.asString());
        if (isDomain) {
            itemValue = lowercaseRegistrationMailPolicyValue(std::move(itemValue));
        }
        const bool valid = isDomain
            ? isSafeRegistrationMailDomain(itemValue)
            : isSafeRegistrationMailProviderName(itemValue);
        if (!valid || !seen.insert(itemValue).second) {
            return false;
        }
        parsed.push_back(std::move(itemValue));
    }

    output = std::move(parsed);
    return true;
}

inline const Json::Value* getRegistrationMailPolicyConfig(const Json::Value& customConfig)
{
    if (!customConfig.isObject() || !customConfig.isMember("nexos_registration")) {
        return nullptr;
    }
    const auto& registration = customConfig["nexos_registration"];
    if (!registration.isObject() || !registration.isMember("mail_policy")) {
        return nullptr;
    }
    const auto& mailPolicy = registration["mail_policy"];
    return mailPolicy.isObject() ? &mailPolicy : nullptr;
}

// Validate only an explicitly configured policy.  With no policy configured,
// callers use the known-safe defaults above.
inline bool validateRegistrationMailPolicy(
    const Json::Value& customConfig,
    std::string* errorMessage = nullptr)
{
    if (!customConfig.isObject() || !customConfig.isMember("nexos_registration")) {
        return true;
    }
    const auto& registration = customConfig["nexos_registration"];
    if (!registration.isObject()) {
        if (errorMessage) *errorMessage = "nexos_registration 必须为 object";
        return false;
    }
    if (!registration.isMember("mail_policy")) {
        return true;
    }
    const auto& policy = registration["mail_policy"];
    if (!policy.isObject()) {
        if (errorMessage) *errorMessage = "nexos_registration.mail_policy 必须为 object";
        return false;
    }

    std::vector<std::string> parsed;
    if (policy.isMember("providers") &&
        !parseRegistrationMailStringList(policy["providers"], false, parsed)) {
        if (errorMessage) {
            *errorMessage = "nexos_registration.mail_policy.providers 必须为非空且不重复的 provider 名称数组";
        }
        return false;
    }
    if (policy.isMember("domain_preference") &&
        !parseRegistrationMailStringList(policy["domain_preference"], true, parsed)) {
        if (errorMessage) {
            *errorMessage = "nexos_registration.mail_policy.domain_preference 必须为非空且不重复的域名数组";
        }
        return false;
    }
    if (policy.isMember("expiry_time_ms") &&
        (!policy["expiry_time_ms"].isInt() || policy["expiry_time_ms"].asInt() <= 0)) {
        if (errorMessage) {
            *errorMessage = "nexos_registration.mail_policy.expiry_time_ms 必须为正整数";
        }
        return false;
    }
    return true;
}

inline RegistrationMailPolicy resolveRegistrationMailPolicy(const Json::Value& customConfig)
{
    RegistrationMailPolicy result;
    const auto* policy = getRegistrationMailPolicyConfig(customConfig);
    if (policy == nullptr) {
        return result;
    }

    std::vector<std::string> parsed;
    if (policy->isMember("providers") &&
        parseRegistrationMailStringList((*policy)["providers"], false, parsed)) {
        result.providers = std::move(parsed);
    }
    if (policy->isMember("domain_preference") &&
        parseRegistrationMailStringList((*policy)["domain_preference"], true, parsed)) {
        result.domainPreference = std::move(parsed);
    }
    if (policy->isMember("expiry_time_ms") &&
        (*policy)["expiry_time_ms"].isInt() &&
        (*policy)["expiry_time_ms"].asInt() > 0) {
        result.expiryTimeMs = (*policy)["expiry_time_ms"].asInt();
    }
    return result;
}

}  // namespace nexos
