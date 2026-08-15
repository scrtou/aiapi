#include <application/generation/protocol/openai/OpenAiErrorFormatter.h>

#include <application/generation/contracts/GenerationEvent.h>

namespace generation::protocol::openai {

std::string errorType(platform::ErrorCode code)
{
    return generation::errorCodeToString(code);
}

Json::Value formatErrorObject(platform::ErrorCode code,
                              const std::string& message,
                              const std::string& detail)
{
    const auto type = errorType(code);
    Json::Value error(Json::objectValue);
    error["message"] = message.empty() ? "Internal server error" : message;
    error["type"] = type;
    error["code"] = type;
    if (!detail.empty()) error["detail"] = detail;
    return error;
}

Json::Value formatError(platform::ErrorCode code,
                        const std::string& message,
                        const std::string& detail)
{
    Json::Value body(Json::objectValue);
    body["error"] = formatErrorObject(code, message, detail);
    return body;
}

}  // namespace generation::protocol::openai
