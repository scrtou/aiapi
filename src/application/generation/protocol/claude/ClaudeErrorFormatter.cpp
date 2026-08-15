#include <application/generation/protocol/claude/ClaudeErrorFormatter.h>

namespace generation::protocol::claude {

std::string errorType(platform::ErrorCode code)
{
    switch (code) {
        case platform::ErrorCode::BadRequest: return "invalid_request_error";
        case platform::ErrorCode::Unauthorized: return "authentication_error";
        case platform::ErrorCode::Forbidden: return "permission_error";
        case platform::ErrorCode::NotFound: return "not_found_error";
        case platform::ErrorCode::RateLimited: return "rate_limit_error";
        case platform::ErrorCode::Timeout: return "overloaded_error";
        case platform::ErrorCode::Conflict: return "invalid_request_error";
        case platform::ErrorCode::ProviderError:
        case platform::ErrorCode::Cancelled:
        case platform::ErrorCode::Internal:
        case platform::ErrorCode::None:
            return "api_error";
    }
    return "api_error";
}

Json::Value formatError(platform::ErrorCode code, const std::string& message)
{
    Json::Value body(Json::objectValue);
    body["type"] = "error";
    body["error"]["type"] = errorType(code);
    body["error"]["message"] = message.empty() ? "Internal server error" : message;
    return body;
}

Json::Value formatApiError(const aiapi::Error& error)
{
    platform::ErrorCode code = platform::ErrorCode::Internal;
    switch (error.httpStatus) {
        case 400: code = platform::ErrorCode::BadRequest; break;
        case 401: code = platform::ErrorCode::Unauthorized; break;
        case 403: code = platform::ErrorCode::Forbidden; break;
        case 404: code = platform::ErrorCode::NotFound; break;
        case 409: code = platform::ErrorCode::Conflict; break;
        case 429: code = platform::ErrorCode::RateLimited; break;
        case 503:
        case 504: code = platform::ErrorCode::Timeout; break;
        case 502: code = platform::ErrorCode::ProviderError; break;
        default: break;
    }
    return formatError(code, error.message);
}

Json::Value formatRateLimitError()
{
    return formatError(platform::ErrorCode::RateLimited, "Rate limit exceeded");
}

}  // namespace generation::protocol::claude
