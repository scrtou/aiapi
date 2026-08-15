#pragma once

#include <platform/result/Error.h>

#include <json/json.h>

#include <string>

namespace generation::protocol::openai {

std::string errorType(platform::ErrorCode code);
Json::Value formatErrorObject(platform::ErrorCode code,
                              const std::string& message,
                              const std::string& detail = {});
Json::Value formatError(platform::ErrorCode code,
                        const std::string& message,
                        const std::string& detail = {});

}  // namespace generation::protocol::openai
