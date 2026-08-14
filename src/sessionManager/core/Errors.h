#ifndef ERRORS_H
#define ERRORS_H

#include <platform/result/Error.h>

#include <string>

/**
 * Compatibility names for the pre-P6 generation pipeline.
 *
 * P6-W1 makes platform::ErrorCode/platform::Error the only cross-layer error
 * model.  The legacy pipeline keeps this namespace while P6-W2/P7 replace its
 * signatures, so callers do not gain a second conversion path in the interim.
 */
namespace error {

using ErrorCode = platform::ErrorCode;
using AppError = platform::Error;

inline std::string errorCodeToString(ErrorCode code)
{
    return std::string(platform::errorCodeName(code));
}

inline int errorCodeToHttpStatus(ErrorCode code)
{
    return platform::defaultHttpStatus(code);
}

/** Convert the legacy numeric generation mapping at its one remaining edge. */
inline AppError fromGenerationError(int genCode, const std::string& message)
{
    switch (genCode) {
        case 0: return AppError::internal(message);
        case 1: return AppError::providerError(message);
        case 2: return AppError::unauthorized(message);
        case 3: return AppError::rateLimited(message);
        case 4: return AppError::badRequest(message);
        case 5: return AppError::providerError(message);
        case 6: return AppError::timeout(message);
        case 7: return AppError::cancelled(message);
        default: return AppError::internal(message);
    }
}

}  // namespace error

#endif  // ERRORS_H
