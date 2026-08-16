#include <infrastructure/provider/chayns/ChaynsProtocolClient.h>

#include <drogon/drogon.h>
#include <infrastructure/provider/chayns/ChaynsBrowserImpersonation.h>
#include <infrastructure/provider/chayns/ChaynsPollingPolicy.h>
#include <platform/Log.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace {

std::string summarizeResponse(const drogon::HttpResponsePtr& response)
{
    if (!response) return "responsePresent=false";
    return "responsePresent=true, status=" +
           std::to_string(static_cast<int>(response->statusCode())) +
           ", bodySize=" + std::to_string(response->getBody().size());
}

platform::Error classifyChaynsHttpError(int status, const std::string& operation)
{
    platform::ErrorCode code = platform::ErrorCode::ProviderError;
    if (status == 401) code = platform::ErrorCode::Unauthorized;
    else if (status == 403) code = platform::ErrorCode::Forbidden;
    else if (status == 404 || status == 422) code = platform::ErrorCode::NotFound;
    else if (status == 408 || status == 504) code = platform::ErrorCode::Timeout;
    else if (status == 429) code = platform::ErrorCode::RateLimited;
    return platform::Error(code,
                           "Chayns " + operation + " rejected",
                           {},
                           "http_rejected",
                           status);
}

platform::Error classifyTransportFailure(drogon::ReqResult result,
                                          const provider::ProviderCallContext& context,
                                          const std::string& operation)
{
    if (context.isCancelled()) {
        return platform::Error::cancelled("Chayns " + operation + " cancelled");
    }
    if (context.deadlineExceeded()) {
        return platform::Error::timeout("Chayns " + operation + " deadline exceeded");
    }
    return platform::Error::providerError(
        "Chayns " + operation + " transport failed",
        "transport_failure",
        0,
        "reqResult=" + std::to_string(static_cast<int>(result)));
}

}  // namespace

namespace chayns {

ChaynsProtocolClient::ChaynsProtocolClient(
    std::shared_ptr<IChaynsHttpTransport> transport)
    : m_transport(std::move(transport))
{
    if (!m_transport) {
        throw std::invalid_argument("ChaynsProtocolClient requires a non-null transport");
    }
}

std::string ChaynsProtocolClient::uploadImage(
    const ImageInfo& image,
    const std::string& personId,
    const std::string& authToken,
    const std::string& accountUserName,
    const std::string& origin,
    const std::string& referer,
    const provider::ProviderCallContext& context,
    std::string_view requestId,
    std::string_view conversationId) const
{
    LOG_INFO << "[chaynsAPI][Protocol] image upload: provider=chaynsapi"
             << ", requestId=" << requestId
             << ", conversationId=" << conversationId
             << ", personIdPresent="
             << !personId.empty();
    if (!image.uploadedUrl.empty()) return image.uploadedUrl;
    if (image.base64Data.empty()) {
        LOG_ERROR << "[chaynsAPI][Protocol] image upload skipped: provider=chaynsapi"
                  << ", requestId=" << requestId
                  << ", conversationId=" << conversationId
                  << ", dataMissing=true";
        return {};
    }

    const std::string uploadPath = "/image-service/v3/Images/" + personId;
    const std::string decodedData = drogon::utils::base64Decode(image.base64Data);
    std::string extension = "png";
    if (image.mediaType.find("jpeg") != std::string::npos ||
        image.mediaType.find("jpg") != std::string::npos) {
        extension = "jpg";
    } else if (image.mediaType.find("gif") != std::string::npos) {
        extension = "gif";
    } else if (image.mediaType.find("webp") != std::string::npos) {
        extension = "webp";
    }

    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::HttpMethod::Post);
    request->setPath(uploadPath);
    request->addHeader("Authorization", "Bearer " + authToken);
    chayns_browser::applyBrowserHeaders(
        request, chayns_browser::accountKeyFor(accountUserName, personId), origin, referer);

    const std::string boundary = "----WebKitFormBoundary" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    request->setContentTypeString("multipart/form-data; boundary=" + boundary);
    const std::string mimeType = extension == "jpg" ? "image/jpeg" : "image/" + extension;

    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"image." +
            extension + "\"\r\n";
    body += "Content-Type: " + mimeType + "\r\n\r\n";
    body += decodedData;
    body += "\r\n--" + boundary + "--\r\n";
    request->setBody(body);

    const double remainingSeconds = static_cast<double>(context.remaining().count()) / 1000.0;
    if (remainingSeconds <= 0.0 || context.isCancelled()) return {};
    const auto [result, response] = m_transport->send(
        "https://cube.tobit.cloud", request,
        std::min(kUpstreamUploadTimeoutSeconds, remainingSeconds));
    if (result != drogon::ReqResult::Ok || !response ||
        (response->statusCode() != drogon::k200OK &&
         response->statusCode() != drogon::k201Created)) {
        LOG_ERROR << "[chaynsAPI][Protocol] image upload failed: provider=chaynsapi"
                  << ", requestId=" << requestId
                  << ", conversationId=" << conversationId << ", "
                  << summarizeResponse(response);
        return {};
    }

    const auto json = response->getJsonObject();
    if (!json) return {};
    if (json->isMember("baseDomain") && json->isMember("image") &&
        (*json)["image"].isMember("path")) {
        return (*json)["baseDomain"].asString() + (*json)["image"]["path"].asString();
    }
    return {};
}

std::optional<Json::Value> ChaynsProtocolClient::getThreadMessages(
    const std::string& threadId,
    const std::string& afterDate,
    const Accountinfo_st& account,
    const policy::RequestRoute& route,
    double timeoutSeconds,
    std::string_view requestId,
    std::string_view conversationId) const
{
    if (timeoutSeconds <= 0.0) return std::nullopt;

    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::HttpMethod::Get);
    request->setPath("/intercom-backend/v2/thread/" + threadId + "/message");
    request->setParameter("take", "1000");
    request->setParameter("viewMode", "user");
    request->setParameter("afterDate", afterDate);
    request->addHeader("Authorization", "Bearer " + account.authToken);
    chayns_browser::applyBrowserHeadersForAccount(
        request, account.userName, account.personId, route.origin, route.referer);

    const auto [result, response] = m_transport->send(
        "https://cube.tobit.cloud", request, timeoutSeconds);
    if (result != drogon::ReqResult::Ok || !response) {
        LOG_WARN << "[chaynsAPI][Protocol] message fetch failed: provider=chaynsapi"
                 << ", requestId=" << requestId
                 << ", conversationId=" << conversationId
                 << ", upstreamThreadId=" << threadId << ", "
                 << summarizeResponse(response);
        return std::nullopt;
    }

    // A 204 is the normal polling response while the upstream thread has no
    // new messages yet.  Treat it as an empty batch and keep it out of WARN
    // logs; the polling loop already records the aggregate result once.
    if (response->statusCode() == drogon::k204NoContent) {
        return Json::Value(Json::arrayValue);
    }
    if (response->statusCode() != drogon::k200OK) {
        LOG_WARN << "[chaynsAPI][Protocol] message fetch failed: provider=chaynsapi"
                 << ", requestId=" << requestId
                 << ", conversationId=" << conversationId
                 << ", upstreamThreadId=" << threadId << ", "
                 << summarizeResponse(response);
        return std::nullopt;
    }

    const auto json = response->getJsonObject();
    if (!json || !json->isArray()) {
        LOG_WARN << "[chaynsAPI][Protocol] message fetch decode failed: provider=chaynsapi"
                 << ", requestId=" << requestId
                 << ", conversationId=" << conversationId
                 << ", upstreamThreadId=" << threadId;
        return std::nullopt;
    }
    return *json;
}

std::optional<std::string> ChaynsProtocolClient::getPersonId(
    const Accountinfo_st& account,
    const policy::RequestRoute& route,
    const provider::ProviderCallContext& context,
    std::string_view requestId,
    std::string_view conversationId) const
{
    const double remainingSeconds =
        static_cast<double>(context.remaining().count()) / 1000.0;
    if (remainingSeconds <= 0.0 || context.isCancelled()) return std::nullopt;

    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::HttpMethod::Get);
    request->setPath("/v2/userSettings");
    request->addHeader("Authorization", "Bearer " + account.authToken);
    chayns_browser::applyBrowserHeadersForAccount(
        request, account.userName, account.personId, route.origin, route.referer);

    const auto [result, response] = m_transport->send(
        "https://auth.chayns.net", request,
        std::min(kUpstreamRequestTimeoutSeconds, remainingSeconds));
    if (result != drogon::ReqResult::Ok || !response ||
        response->statusCode() != drogon::k200OK) {
        LOG_WARN << "[chaynsAPI][Protocol] user settings fetch failed: provider=chaynsapi"
                 << ", requestId=" << requestId
                 << ", conversationId=" << conversationId << ", "
                 << summarizeResponse(response);
        return std::nullopt;
    }

    const auto json = response->getJsonObject();
    if (!json || !json->isMember("personId") || !(*json)["personId"].isString()) {
        LOG_WARN << "[chaynsAPI][Protocol] user settings decode failed: provider=chaynsapi"
                 << ", requestId=" << requestId
                 << ", conversationId=" << conversationId;
        return std::nullopt;
    }
    auto personId = (*json)["personId"].asString();
    if (personId.empty()) return std::nullopt;
    return personId;
}

namespace {

ChaynsProtocolClient::MessageSubmission submitMessage(
    const std::shared_ptr<IChaynsHttpTransport>& transport,
    const drogon::HttpRequestPtr& request,
    const provider::ProviderCallContext& context,
    std::string_view requestId,
    std::string_view conversationId,
    bool parseThreadMembers,
    const std::string& userPersonId,
    const std::string& modelPersonId)
{
    ChaynsProtocolClient::MessageSubmission submission;
    const double remainingSeconds =
        static_cast<double>(context.remaining().count()) / 1000.0;
    if (remainingSeconds <= 0.0 || context.isCancelled()) {
        submission.ambiguous = true;
        submission.error = context.isCancelled()
            ? platform::Error::cancelled("Chayns message submission cancelled")
            : platform::Error::timeout("Chayns message submission deadline exceeded");
        return submission;
    }

    const auto [result, response] = transport->send(
        "https://cube.tobit.cloud", request,
        std::min(kUpstreamRequestTimeoutSeconds, remainingSeconds));
    submission.transportOk = result == drogon::ReqResult::Ok && response;
    submission.statusCode = response ? static_cast<int>(response->statusCode()) : 0;
    if (!submission.transportOk) {
        submission.ambiguous = true;
        submission.error = classifyTransportFailure(
            result, context, "message submission");
        LOG_ERROR << "[chaynsAPI][Protocol] message submission transport failed: "
                  << "provider=chaynsapi, requestId=" << requestId
                  << ", conversationId=" << conversationId << ", "
                  << summarizeResponse(response);
        return submission;
    }
    if (submission.statusCode != static_cast<int>(drogon::k200OK) &&
        submission.statusCode != static_cast<int>(drogon::k201Created)) {
        submission.ambiguous = policy::postFailureMayHaveBeenAccepted(
            submission.statusCode);
        submission.error = classifyChaynsHttpError(
            submission.statusCode, "message submission");
        LOG_ERROR << "[chaynsAPI][Protocol] message submission rejected: "
                  << "provider=chaynsapi, requestId=" << requestId
                  << ", conversationId=" << conversationId
                  << ", status=" << submission.statusCode;
        return submission;
    }

    const auto json = response->getJsonObject();
    if (!json || !json->isObject()) {
        submission.ambiguous = true;
        submission.error = platform::Error::providerError(
            "Chayns message submission returned invalid JSON",
            "invalid_json",
            submission.statusCode);
        LOG_ERROR << "[chaynsAPI][Protocol] message submission decode failed: "
                  << "provider=chaynsapi, requestId=" << requestId
                  << ", conversationId=" << conversationId;
        return submission;
    }
    submission.accepted = true;
    if (parseThreadMembers && json->isMember("id")) {
        submission.threadId = (*json)["id"].asString();
        if (json->isMember("members") && (*json)["members"].isArray()) {
            for (const auto& member : (*json)["members"]) {
                if (member.get("personId", "").asString() == userPersonId) {
                    submission.userAuthorId = member.get("id", "").asString();
                }
                if (member.get("personId", "").asString() == modelPersonId) {
                    submission.agentAuthorId = member.get("id", "").asString();
                }
            }
        }
        if (json->isMember("messages") && (*json)["messages"].isArray() &&
            !(*json)["messages"].empty()) {
            const auto& sent = (*json)["messages"][0];
            submission.creationTime = sent.get("creationTime", "").asString();
            submission.messageId = sent.get("id", "").asString();
            if (sent.isMember("author") && sent["author"].isObject()) {
                submission.userAuthorId =
                    sent["author"].get("id", submission.userAuthorId).asString();
            }
        }
    } else {
        submission.creationTime = json->get("creationTime", "").asString();
        submission.messageId = json->get("id", "").asString();
        if (json->isMember("author") && (*json)["author"].isObject()) {
            submission.userAuthorId = (*json)["author"].get("id", "").asString();
        }
    }
    return submission;
}

}  // namespace

ChaynsProtocolClient::MessageSubmission ChaynsProtocolClient::sendFollowupMessage(
    const std::string& threadId,
    const Json::Value& body,
    const Accountinfo_st& account,
    const policy::RequestRoute& route,
    const provider::ProviderCallContext& context,
    std::string_view requestId,
    std::string_view conversationId) const
{
    auto request = drogon::HttpRequest::newHttpJsonRequest(body);
    request->setMethod(drogon::HttpMethod::Post);
    request->setPath("/intercom-backend/v2/thread/" + threadId + "/message");
    request->addHeader("Authorization", "Bearer " + account.authToken);
    chayns_browser::applyBrowserHeadersForAccount(
        request, account.userName, account.personId, route.origin, route.referer);
    return submitMessage(m_transport, request, context, requestId, conversationId,
                          false, {}, {});
}

ChaynsProtocolClient::MessageSubmission ChaynsProtocolClient::createThread(
    const Json::Value& body,
    const Accountinfo_st& account,
    const policy::RequestRoute& route,
    const std::string& userPersonId,
    const std::string& modelPersonId,
    const provider::ProviderCallContext& context,
    std::string_view requestId,
    std::string_view conversationId) const
{
    auto request = drogon::HttpRequest::newHttpJsonRequest(body);
    request->setMethod(drogon::HttpMethod::Post);
    request->setPath("/intercom-backend/v2/thread?forceCreate=true");
    request->addHeader("Authorization", "Bearer " + account.authToken);
    chayns_browser::applyBrowserHeadersForAccount(
        request, account.userName, account.personId, route.origin, route.referer);
    return submitMessage(m_transport, request, context, requestId, conversationId,
                          true, userPersonId, modelPersonId);
}

std::optional<Json::Value> ChaynsProtocolClient::getModelCatalog(
    const provider::ProviderCallContext* context) const
{
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::HttpMethod::Get);
    request->setPath("/chayns-ai-chatbot/nativeModelChatbot");
    chayns_browser::applyBrowserHeaders(request);

    double timeoutSeconds = kUpstreamRequestTimeoutSeconds;
    if (context) {
        const double remainingSeconds =
            static_cast<double>(context->remaining().count()) / 1000.0;
        if (remainingSeconds <= 0.0 || context->isCancelled()) return std::nullopt;
        timeoutSeconds = std::min(timeoutSeconds, remainingSeconds);
    }

    const auto [result, response] = m_transport->send(
        "https://cube.tobit.cloud", request, timeoutSeconds);
    if (result != drogon::ReqResult::Ok || !response ||
        response->statusCode() != drogon::k200OK) {
        LOG_ERROR << "[chaynsAPI][Protocol] model catalog fetch failed: provider=chaynsapi, "
                  << summarizeResponse(response);
        return std::nullopt;
    }
    const auto json = response->getJsonObject();
    if (!json) {
        LOG_ERROR << "[chaynsAPI][Protocol] model catalog decode failed: provider=chaynsapi";
        return std::nullopt;
    }
    return *json;
}

platform::Result<void> ChaynsProtocolClient::deleteThread(
    const Accountinfo_st& account,
    const std::string& threadId,
    const std::string& origin,
    const std::string& referer) const
{
    Json::Value body;
    Json::Value threadIds(Json::arrayValue);
    threadIds.append(threadId);
    body["threadIds"] = std::move(threadIds);
    body["personId"] = account.personId;

    auto request = drogon::HttpRequest::newHttpJsonRequest(body);
    request->setMethod(drogon::HttpMethod::Delete);
    request->setPath("/intercom-backend/v2/thread/member/delete");
    request->addHeader("Authorization", "Bearer " + account.authToken);
    chayns_browser::applyBrowserHeadersForAccount(
        request, account.userName, account.personId,
        origin.empty() ? policy::kFreeOrigin : origin,
        referer.empty() ? policy::kFreeReferer : referer);

    const auto [result, response] = m_transport->send(
        "https://cube.tobit.cloud", request, kUpstreamRequestTimeoutSeconds);
    if (result != drogon::ReqResult::Ok || !response ||
        (response->statusCode() != drogon::k200OK &&
         response->statusCode() != drogon::k204NoContent)) {
        const int status = response ? static_cast<int>(response->statusCode()) : 0;
        LOG_WARN << "[chaynsAPI][Protocol] thread delete failed: provider=chaynsapi"
                 << ", upstreamThreadId=" << threadId << ", "
                 << summarizeResponse(response);
        return platform::Result<void>::failure(platform::Error::providerError(
            "Failed to delete Chayns upstream thread", "thread_delete_failed", status));
    }
    return platform::Result<void>::success();
}

}  // namespace chayns
