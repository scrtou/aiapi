#include <drogon/drogon_test.h>

#include "utils/NexosRegistrationMailPolicy.h"

DROGON_TEST(NexosRegistrationMailPolicy_UsesGmailSmailProDefaults)
{
    const Json::Value customConfig(Json::objectValue);
    const auto policy = nexos::resolveRegistrationMailPolicy(customConfig);

    REQUIRE(policy.providers.size() == 1);
    CHECK(policy.providers.front() == "smailpro_web");
    REQUIRE(policy.domainPreference.size() == 1);
    CHECK(policy.domainPreference.front() == "gmail.com");
    CHECK(policy.expiryTimeMs == nexos::kDefaultRegistrationMailExpiryTimeMs);
}

DROGON_TEST(NexosRegistrationMailPolicy_AllowsExplicitOperatorOverride)
{
    Json::Value customConfig(Json::objectValue);
    auto& policyConfig = customConfig["nexos_registration"]["mail_policy"];
    policyConfig["providers"] = Json::arrayValue;
    policyConfig["providers"].append("gptmail");
    policyConfig["domain_preference"] = Json::arrayValue;
    policyConfig["domain_preference"].append("Example.Mail");
    policyConfig["expiry_time_ms"] = 600000;

    std::string validationError;
    CHECK(nexos::validateRegistrationMailPolicy(customConfig, &validationError));
    CHECK(validationError.empty());

    const auto policy = nexos::resolveRegistrationMailPolicy(customConfig);
    REQUIRE(policy.providers.size() == 1);
    CHECK(policy.providers.front() == "gptmail");
    REQUIRE(policy.domainPreference.size() == 1);
    CHECK(policy.domainPreference.front() == "example.mail");
    CHECK(policy.expiryTimeMs == 600000);
}

DROGON_TEST(NexosRegistrationMailPolicy_RejectsMalformedExplicitPolicy)
{
    Json::Value customConfig(Json::objectValue);
    auto& policyConfig = customConfig["nexos_registration"]["mail_policy"];
    policyConfig["providers"] = Json::arrayValue;
    policyConfig["providers"].append("smailpro_web");
    policyConfig["providers"].append("smailpro_web");
    policyConfig["domain_preference"] = Json::arrayValue;
    policyConfig["domain_preference"].append("gmail.com");

    std::string validationError;
    CHECK(!nexos::validateRegistrationMailPolicy(customConfig, &validationError));
    CHECK(!validationError.empty());

    // The resolver is defensive as well: a malformed field cannot re-enable
    // GPTMail or remove the safe Gmail/SmailPro fallback at runtime.
    const auto policy = nexos::resolveRegistrationMailPolicy(customConfig);
    REQUIRE(policy.providers.size() == 1);
    CHECK(policy.providers.front() == "smailpro_web");
    REQUIRE(policy.domainPreference.size() == 1);
    CHECK(policy.domainPreference.front() == "gmail.com");
}
