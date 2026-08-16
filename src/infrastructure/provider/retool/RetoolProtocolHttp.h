#pragma once

#include <domain/model/ProviderCallContext.h>
#include <infrastructure/provider/retool/RetoolHttpTransport.h>
#include <platform/result/Error.h>
#include <platform/result/Result.h>

#include <drogon/drogon.h>
#include <json/json.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace retool::protocol_http {

/** Map an upstream Retool status to the transport-neutral platform error. */
[[nodiscard]] platform::Error classifyHttpError(int httpStatus,
                                                const std::string& message);

/** Decode a successful JSON response without silently turning malformed wire
 * data into an empty object.  HTTP status mapping remains the caller's
 * responsibility because some provider flows intentionally inspect statuses
 * such as 413 before deciding whether to retry. */
[[nodiscard]] platform::Result<Json::Value> decodeJsonBody(
    const drogon::HttpResponsePtr& response,
    std::string_view operation);

/**
 * Response envelope used by both Retool wire clients.  It intentionally keeps
 * the historical pointer-like read access for orchestration code while making
 * transport failures an explicit platform::Error at the adapter boundary.
 */
class ResponseResult final
{
  public:
    static ResponseResult success(drogon::HttpResponsePtr response)
    {
        ResponseResult result;
        result.response_ = std::move(response);
        return result;
    }

    static ResponseResult failure(platform::Error error)
    {
        ResponseResult result;
        result.error_ = std::move(error);
        return result;
    }

    [[nodiscard]] bool ok() const noexcept { return response_ != nullptr; }
    explicit operator bool() const noexcept { return ok(); }

    drogon::HttpResponse* operator->() const noexcept { return response_.get(); }
    operator const drogon::HttpResponsePtr&() const noexcept { return response_; }

    const drogon::HttpResponsePtr& value() const
    {
        if (!response_) {
            throw std::logic_error("Retool ResponseResult::value on failure");
        }
        return response_;
    }

    const platform::Error& error() const
    {
        if (!error_) {
            throw std::logic_error("Retool ResponseResult::error on success");
        }
        return *error_;
    }

  private:
    drogon::HttpResponsePtr response_;
    std::optional<platform::Error> error_;
};

[[nodiscard]] ResponseResult sendJson(
    const std::shared_ptr<IRetoolHttpTransport>& transport,
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    drogon::HttpMethod method,
    const std::string& path,
    const Json::Value* body,
    const Json::Value& workspace,
    const std::string& operation,
    double timeoutSeconds);

}  // namespace retool::protocol_http
