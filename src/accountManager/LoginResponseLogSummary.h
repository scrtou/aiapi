#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

#include <json/value.h>

// Login-service responses can contain cookies, bearer tokens, passwords, and
// account identifiers.  These helpers deliberately report only response shape
// and presence flags; they never serialize any response value.
namespace account_logging {

inline const char* boolText(bool value)
{
    return value ? "true" : "false";
}

inline bool hasNonEmptyString(const Json::Value& value)
{
    return value.isString() && !value.asString().empty();
}

inline bool hasAnyValue(const Json::Value& value)
{
    return !value.isNull() && (!value.isString() || !value.asString().empty());
}

inline std::string summarizeLoginTransport(int httpStatus,
                                           const std::string& contentType,
                                           std::size_t bodySize)
{
    return "httpStatus=" + std::to_string(httpStatus) +
           ", contentTypePresent=" + boolText(!contentType.empty()) +
           ", bodySize=" + std::to_string(bodySize);
}

inline std::string summarizeLoginResponse(const Json::Value& envelope,
                                          int httpStatus,
                                          const std::string& contentType,
                                          std::size_t bodySize)
{
    const auto& data = envelope["data"];
    const auto& result = data["result"];
    const auto& session = result["session"];
    const auto& identity = result["identity"];
    const auto& account = result["account"];
    const auto& flags = result["flags"];

    const bool sessionCredentialPresent = session.isObject() &&
        (hasNonEmptyString(session["access_token"]) || hasAnyValue(session["cookies"]));
    const bool identityPresent = identity.isObject() &&
        (hasNonEmptyString(identity["external_user_id"]) ||
         hasNonEmptyString(identity["external_subject"]) ||
         hasNonEmptyString(identity["identity_id"]));
    const bool accountPresent = account.isObject() && hasNonEmptyString(account["email"]);

    std::string summary = summarizeLoginTransport(httpStatus, contentType, bodySize);
    summary += std::string(", envelopeSuccess=") + boolText(envelope.get("success", false).asBool());
    summary += std::string(", traceIdPresent=") + boolText(hasNonEmptyString(envelope["trace_id"]));
    summary += std::string(", resultPresent=") + boolText(result.isObject());
    summary += std::string(", sessionPresent=") + boolText(session.isObject());
    summary += std::string(", sessionCredentialPresent=") + boolText(sessionCredentialPresent);
    summary += std::string(", identityPresent=") + boolText(identityPresent);
    summary += std::string(", accountPresent=") + boolText(accountPresent);
    summary += std::string(", emailVerified=") +
        std::string(flags["email_verified"].isBool() ? boolText(flags["email_verified"].asBool()) : "absent");
    summary += std::string(", browserLogin=") +
        std::string(flags["browser_login"].isBool() ? boolText(flags["browser_login"].asBool()) : "absent");
    return summary;
}

inline std::string safeErrorCode(const Json::Value& error)
{
    if (!error["code"].isString()) {
        return "<absent>";
    }

    const std::string code = error["code"].asString();
    const bool isSafeCode = !code.empty() && code.size() <= 64 &&
        std::all_of(code.begin(), code.end(), [](unsigned char character) {
            return std::isupper(character) || std::isdigit(character) || character == '_';
        });
    return isSafeCode ? code : "<redacted>";
}

inline std::string summarizeLoginError(const Json::Value& envelope)
{
    const auto& error = envelope["error"];
    if (!error.isObject()) {
        return "errorEnvelopePresent=false";
    }

    const auto& message = error["message"];
    const std::size_t messageSize = message.isString() ? message.asString().size() : 0;
    return "errorEnvelopePresent=true, errorCode=" + safeErrorCode(error) +
           ", errorMessagePresent=" + boolText(message.isString() && !message.asString().empty()) +
           ", errorMessageSize=" + std::to_string(messageSize);
}

// Workflow task state is useful for operations diagnostics, but it arrives
// from a downstream service and must not be copied to the application log
// verbatim.  Keep only a small, machine-readable identifier alphabet.
inline std::string safeWorkflowField(const Json::Value& value)
{
    if (!value.isString() || value.asString().empty()) {
        return "<absent>";
    }

    const std::string field = value.asString();
    const bool isSafe = field.size() <= 64 &&
        std::all_of(field.begin(), field.end(), [](unsigned char character) {
            return std::islower(character) || std::isdigit(character) ||
                   character == '_' || character == '-';
        });
    return isSafe ? field : "<redacted>";
}

// A workflow result may contain registered-account credentials, email
// verification links, cookies, and access tokens.  Report only its envelope
// shape and the bounded task identifiers needed to diagnose control flow.
inline std::string summarizeWorkflowEnvelope(const Json::Value& envelope)
{
    const auto& data = envelope["data"];
    const auto& task = data["task"];
    const auto& error = data["error"];

    std::string summary = "envelopeSuccess=" +
        std::string(boolText(envelope.get("success", false).asBool()));
    summary += ", dataPresent=" + std::string(boolText(data.isObject()));
    summary += ", taskPresent=" + std::string(boolText(task.isObject()));
    summary += ", taskStatus=" + safeWorkflowField(task["status"]);
    summary += ", taskState=" + safeWorkflowField(task["state"]);
    summary += ", resultPresent=" + std::string(boolText(data["result"].isObject()));
    summary += ", errorPresent=" + std::string(boolText(error.isObject()));
    summary += ", errorCode=" + safeErrorCode(error);
    return summary;
}

inline std::string summarizeParseError(const std::string& parseError)
{
    return "parseErrorPresent=" + std::string(boolText(!parseError.empty())) +
           ", parseErrorSize=" + std::to_string(parseError.size());
}

}  // namespace account_logging
