#pragma once

#include <platform/result/ErrorCode.h>

#include <string>
#include <utility>

namespace platform {

/**
 * Failure value transported by Result across platform/domain/application
 * boundaries.  `detail` is diagnostics-only; transport must not serialize it
 * to an API client.  `upstreamHttpStatus` preserves the provider's observed
 * status independently from the semantic HTTP mapping in httpStatus().
 */
struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;
    std::string providerCode;
    int upstreamHttpStatus = 0;
    std::string detail;

    Error() = default;

    Error(ErrorCode errorCode,
          std::string safeMessage,
          std::string diagnosticDetail = {},
          std::string sourceProviderCode = {},
          int sourceHttpStatus = 0)
        : code(errorCode),
          message(std::move(safeMessage)),
          providerCode(std::move(sourceProviderCode)),
          upstreamHttpStatus(sourceHttpStatus),
          detail(std::move(diagnosticDetail))
    {
    }

    [[nodiscard]] bool hasError() const noexcept { return code != ErrorCode::None; }
    [[nodiscard]] int httpStatus() const noexcept { return defaultHttpStatus(code); }
    [[nodiscard]] std::string type() const { return std::string(errorCodeName(code)); }

    static Error badRequest(std::string message, std::string detail = {})
    {
        return Error(ErrorCode::BadRequest, std::move(message), std::move(detail));
    }

    static Error unauthorized(std::string message = "Unauthorized")
    {
        return Error(ErrorCode::Unauthorized, std::move(message));
    }

    static Error forbidden(std::string message = "Forbidden")
    {
        return Error(ErrorCode::Forbidden, std::move(message));
    }

    static Error notFound(std::string message = "Resource not found")
    {
        return Error(ErrorCode::NotFound, std::move(message));
    }

    static Error conflict(std::string message = "Request conflict")
    {
        return Error(ErrorCode::Conflict, std::move(message));
    }

    static Error rateLimited(std::string message = "Too many requests")
    {
        return Error(ErrorCode::RateLimited, std::move(message));
    }

    static Error timeout(std::string message = "Request timeout")
    {
        return Error(ErrorCode::Timeout, std::move(message));
    }

    static Error providerError(std::string message,
                               std::string providerCode = {},
                               int upstreamHttpStatus = 0,
                               std::string detail = {})
    {
        return Error(ErrorCode::ProviderError,
                     std::move(message),
                     std::move(detail),
                     std::move(providerCode),
                     upstreamHttpStatus);
    }

    static Error internal(std::string message = "Internal server error",
                          std::string detail = {})
    {
        return Error(ErrorCode::Internal, std::move(message), std::move(detail));
    }

    static Error cancelled(std::string message = "Request cancelled")
    {
        return Error(ErrorCode::Cancelled, std::move(message));
    }
};

}  // namespace platform
