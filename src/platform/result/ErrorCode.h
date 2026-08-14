#pragma once

#include <string_view>

namespace platform {

/**
 * Stable, transport-neutral failures used at cross-layer boundaries.
 *
 * `None` exists solely for legacy/default-value interop.  A Result failure
 * must always carry one of the remaining values.
 */
enum class ErrorCode {
    None = 0,
    BadRequest,
    Unauthorized,
    Forbidden,
    NotFound,
    Conflict,
    RateLimited,
    Timeout,
    ProviderError,
    Internal,
    Cancelled,
};

inline constexpr std::string_view errorCodeName(ErrorCode code) noexcept
{
    switch (code) {
        case ErrorCode::None: return "none";
        case ErrorCode::BadRequest: return "bad_request";
        case ErrorCode::Unauthorized: return "unauthorized";
        case ErrorCode::Forbidden: return "forbidden";
        case ErrorCode::NotFound: return "not_found";
        case ErrorCode::Conflict: return "conflict";
        case ErrorCode::RateLimited: return "rate_limited";
        case ErrorCode::Timeout: return "timeout";
        case ErrorCode::ProviderError: return "provider_error";
        case ErrorCode::Internal: return "internal_error";
        case ErrorCode::Cancelled: return "cancelled";
    }
    return "internal_error";
}

/** The one semantic ErrorCode -> HTTP status mapping used by transport. */
inline constexpr int defaultHttpStatus(ErrorCode code) noexcept
{
    switch (code) {
        case ErrorCode::None: return 200;
        case ErrorCode::BadRequest: return 400;
        case ErrorCode::Unauthorized: return 401;
        case ErrorCode::Forbidden: return 403;
        case ErrorCode::NotFound: return 404;
        case ErrorCode::Conflict: return 409;
        case ErrorCode::RateLimited: return 429;
        case ErrorCode::Timeout: return 504;
        case ErrorCode::ProviderError: return 502;
        case ErrorCode::Internal: return 500;
        case ErrorCode::Cancelled: return 499;
    }
    return 500;
}

}  // namespace platform
