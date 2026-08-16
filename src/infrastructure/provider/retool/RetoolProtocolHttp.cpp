#include <infrastructure/provider/retool/RetoolProtocolHttp.h>

#include <algorithm>
#include <sstream>
#include <utility>
#include <vector>

namespace {

std::string cookieHeader(const Json::Value& workspace)
{
    std::vector<std::string> parts;
    const auto append = [&parts](const std::string& name, const std::string& value) {
        if (!value.empty()) parts.push_back(name + "=" + value);
    };
    append("accessToken", workspace.get("accessToken", "").asString());
    append("xsrfToken", workspace.get("xsrfToken", "").asString());
    append("xsrfTokenSameSite", workspace.get("xsrfToken", "").asString());
    if (workspace.isMember("extraCookies") && workspace["extraCookies"].isObject()) {
        for (const auto& name : workspace["extraCookies"].getMemberNames()) {
            append(name, workspace["extraCookies"][name].asString());
        }
    }
    std::ostringstream output;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) output << "; ";
        output << parts[index];
    }
    return output.str();
}

}  // namespace

namespace retool::protocol_http {

platform::Error classifyHttpError(int httpStatus, const std::string& message)
{
    platform::ErrorCode code = platform::ErrorCode::ProviderError;
    if (httpStatus == 401) {
        code = platform::ErrorCode::Unauthorized;
    } else if (httpStatus == 403) {
        code = platform::ErrorCode::Forbidden;
    } else if (httpStatus == 400 || httpStatus == 404 || httpStatus == 422) {
        code = platform::ErrorCode::BadRequest;
    } else if (httpStatus == 408 || httpStatus == 504) {
        code = platform::ErrorCode::Timeout;
    } else if (httpStatus == 429) {
        code = platform::ErrorCode::RateLimited;
    }
    return platform::Error(code, message, {}, {}, httpStatus);
}

platform::Result<Json::Value> decodeJsonBody(
    const drogon::HttpResponsePtr& response,
    std::string_view operation)
{
    const std::string prefix = "Retool " + std::string(operation);
    if (!response) {
        return platform::Result<Json::Value>::failure(
            platform::Error::providerError(prefix + " response is missing",
                                           "response_missing"));
    }

    if (const auto json = response->getJsonObject()) {
        return platform::Result<Json::Value>::success(*json);
    }

    Json::Value parsed;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream input(std::string(response->getBody()));
    if (Json::parseFromStream(reader, input, &parsed, &errors)) {
        return platform::Result<Json::Value>::success(std::move(parsed));
    }

    return platform::Result<Json::Value>::failure(
        platform::Error::providerError(
            prefix + " response contains invalid JSON",
            "invalid_json",
            static_cast<int>(response->statusCode()),
            errors));
}

ResponseResult sendJson(
    const std::shared_ptr<IRetoolHttpTransport>& transport,
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    drogon::HttpMethod method,
    const std::string& path,
    const Json::Value* body,
    const Json::Value& workspace,
    const std::string& operation,
    double timeoutSeconds)
{
    const double remainingSeconds =
        static_cast<double>(context.remaining().count()) / 1000.0;
    if (!transport) {
        return ResponseResult::failure(platform::Error::internal(
            "Retool transport is unavailable"));
    }
    if (context.isCancelled()) {
        return ResponseResult::failure(platform::Error::cancelled(
            "Retool " + operation + " cancelled"));
    }
    if (remainingSeconds <= 0.0) {
        return ResponseResult::failure(platform::Error::timeout(
            "Retool " + operation + " deadline exceeded"));
    }

    auto request = body ? drogon::HttpRequest::newHttpJsonRequest(*body)
                        : drogon::HttpRequest::newHttpRequest();
    request->setMethod(method);
    request->setPath(path);
    request->addHeader("accept", "application/json");
    request->addHeader("content-type", "application/json");
    request->addHeader("x-xsrf-token", workspace.get("xsrfToken", "").asString());
    request->addHeader("x-retool-client-version", "3.356.0-f7a1e09 (Build 313746)");
    request->addHeader("user-agent", "Mozilla/5.0");
    request->addHeader("cookie", cookieHeader(workspace));
    const auto [result, response] = transport->send(
        baseUrl, request, std::min(timeoutSeconds, remainingSeconds));
    if (context.isCancelled()) {
        return ResponseResult::failure(platform::Error::cancelled(
            "Retool " + operation + " cancelled"));
    }
    if (context.deadlineExceeded()) {
        return ResponseResult::failure(platform::Error::timeout(
            "Retool " + operation + " deadline exceeded"));
    }
    if (result != drogon::ReqResult::Ok || !response) {
        return ResponseResult::failure(platform::Error::providerError(
            "Retool " + operation + " transport failed"));
    }
    return ResponseResult::success(std::move(response));
}

}  // namespace retool::protocol_http
