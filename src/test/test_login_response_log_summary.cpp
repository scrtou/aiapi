#include <drogon/drogon_test.h>

#include <application/account/LoginResponseLogSummary.h>

DROGON_TEST(LoginResponseLogSummary_ReportsOnlyPresenceFlags)
{
    Json::Value response(Json::objectValue);
    response["success"] = true;
    response["trace_id"] = "trace-should-not-be-logged";
    auto& result = response["data"]["result"];
    result["session"]["access_token"] = "access-token-should-not-be-logged";
    result["session"]["cookies"] = Json::arrayValue;
    result["session"]["cookies"].append("cookie-should-not-be-logged");
    result["identity"]["external_user_id"] = "identity-should-not-be-logged";
    result["account"]["email"] = "account-should-not-be-logged@example.test";
    result["flags"]["email_verified"] = true;
    result["flags"]["browser_login"] = true;

    const auto summary = account_logging::summarizeLoginResponse(
        response, 200, "application/json", 1234);

    CHECK(summary.find("httpStatus=200") != std::string::npos);
    CHECK(summary.find("bodySize=1234") != std::string::npos);
    CHECK(summary.find("envelopeSuccess=true") != std::string::npos);
    CHECK(summary.find("traceIdPresent=true") != std::string::npos);
    CHECK(summary.find("sessionCredentialPresent=true") != std::string::npos);
    CHECK(summary.find("identityPresent=true") != std::string::npos);
    CHECK(summary.find("accountPresent=true") != std::string::npos);
    CHECK(summary.find("emailVerified=true") != std::string::npos);
    CHECK(summary.find("browserLogin=true") != std::string::npos);

    CHECK(summary.find("trace-should-not-be-logged") == std::string::npos);
    CHECK(summary.find("access-token-should-not-be-logged") == std::string::npos);
    CHECK(summary.find("cookie-should-not-be-logged") == std::string::npos);
    CHECK(summary.find("identity-should-not-be-logged") == std::string::npos);
    CHECK(summary.find("account-should-not-be-logged@example.test") == std::string::npos);
}

DROGON_TEST(LoginResponseLogSummary_RedactsUntrustedErrorValues)
{
    Json::Value response(Json::objectValue);
    response["error"]["code"] = "token-value-should-not-be-logged";
    response["error"]["message"] = "password=should-not-be-logged";

    const auto summary = account_logging::summarizeLoginError(response);

    CHECK(summary.find("errorEnvelopePresent=true") != std::string::npos);
    CHECK(summary.find("errorCode=<redacted>") != std::string::npos);
    CHECK(summary.find("errorMessagePresent=true") != std::string::npos);
    CHECK(summary.find("token-value-should-not-be-logged") == std::string::npos);
    CHECK(summary.find("password=should-not-be-logged") == std::string::npos);
}

DROGON_TEST(LoginResponseLogSummary_WorkflowEnvelopeOmitsCredentials)
{
    Json::Value workflow(Json::objectValue);
    workflow["success"] = true;
    workflow["data"]["task"]["status"] = "failed";
    workflow["data"]["task"]["state"] = "create_account";
    workflow["data"]["result"]["registration"]["account"]["email"] =
        "account-should-not-be-logged@example.test";
    workflow["data"]["result"]["registration"]["account"]["password"] =
        "password-should-not-be-logged";
    workflow["data"]["result"]["login"]["session"]["access_token"] =
        "token-should-not-be-logged";
    workflow["data"]["error"]["code"] = "MAIL_CREATE_FAILED";
    workflow["data"]["error"]["message"] = "secret response should not be logged";

    const auto summary = account_logging::summarizeWorkflowEnvelope(workflow);

    CHECK(summary.find("envelopeSuccess=true") != std::string::npos);
    CHECK(summary.find("taskPresent=true") != std::string::npos);
    CHECK(summary.find("taskStatus=failed") != std::string::npos);
    CHECK(summary.find("taskState=create_account") != std::string::npos);
    CHECK(summary.find("resultPresent=true") != std::string::npos);
    CHECK(summary.find("errorCode=MAIL_CREATE_FAILED") != std::string::npos);
    CHECK(summary.find("account-should-not-be-logged@example.test") == std::string::npos);
    CHECK(summary.find("password-should-not-be-logged") == std::string::npos);
    CHECK(summary.find("token-should-not-be-logged") == std::string::npos);
    CHECK(summary.find("secret response should not be logged") == std::string::npos);
}
