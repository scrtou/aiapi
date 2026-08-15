#pragma once

#include <domain/model/AiApiData.h>
#include <platform/result/Error.h>

#include <json/json.h>

#include <string>

namespace generation::protocol::claude {

std::string errorType(platform::ErrorCode code);
Json::Value formatError(platform::ErrorCode code, const std::string& message);
Json::Value formatApiError(const aiapi::Error& error);
Json::Value formatRateLimitError();

}  // namespace generation::protocol::claude
