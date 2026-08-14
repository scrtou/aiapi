#include <controllers/RetoolWorkspaceController.h>
#include <controllers/codecs/ChannelJsonCodec.h>
#include <retoolWorkspace/RetoolWorkspaceJsonCodec.h>

#include <controllers/ControllerUtils.h>
#include <optional>
#include <stdexcept>
#include <domain/model/RetoolWorkspaceInfo.h>

using namespace drogon;

workspace::IRetoolWorkspaceAdminUseCase* RetoolWorkspaceController::useCase_ = nullptr;

void RetoolWorkspaceController::setUseCase(
    workspace::IRetoolWorkspaceAdminUseCase* workspaces)
{
    useCase_ = workspaces;
}

namespace
{
std::optional<std::string> getWorkspaceId(const HttpRequestPtr& req, const Json::Value* body = nullptr)
{
    const auto queryId = req->getParameter("workspaceId");
    if (!queryId.empty()) return queryId;
    if (body && body->isObject() && body->isMember("workspaceId"))
    {
        return (*body)["workspaceId"].asString();
    }
    return std::nullopt;
}

}  // namespace

void RetoolWorkspaceController::createWorkspace(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;
    try
    {
        if (!useCase_) throw std::runtime_error("workspace admin use case unavailable");
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        auto workspace = useCase_->provision(Json::writeString(builder, *jsonPtr));
        Json::Value response(Json::objectValue);
        response["status"] = "success";
        response["workspace"] = retoolworkspacecodec::toJson(workspace, true);
        ctl::sendJson(callback, response, k200OK);
    }
    catch (const std::exception& ex)
    {
        ctl::sendError(callback, k500InternalServerError, "workspace_create_failed", ex.what());
    }
}

void RetoolWorkspaceController::upsertWorkspace(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;

    RetoolWorkspaceInfo info = retoolworkspacecodec::fromJson(*jsonPtr);
    if (info.workspaceId.empty())
    {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "workspaceId is required");
        return;
    }

    std::string error;
    if (!useCase_ || !useCase_->upsert(info, &error))
    {
        if (error == "baseUrl is required") {
            ctl::sendError(callback, k400BadRequest, "invalid_request_error", error);
            return;
        }
        ctl::sendError(callback, k500InternalServerError, "workspace_upsert_failed", error);
        return;
    }

    Json::Value response(Json::objectValue);
    response["status"] = "success";
    response["workspace"] = retoolworkspacecodec::toJson(info, true);
    ctl::sendJson(callback, response, k200OK);
}

void RetoolWorkspaceController::workspaceInfo(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    const auto workspaceId = getWorkspaceId(req);
    if (!workspaceId || workspaceId->empty())
    {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "workspaceId is required");
        return;
    }

    std::string error;
    auto workspace = useCase_ ? useCase_->get(*workspaceId, &error) : std::optional<RetoolWorkspaceInfo>{};
    if (!workspace)
    {
        ctl::sendError(callback, k404NotFound, "not_found", error.empty() ? "workspace not found" : error);
        return;
    }

    Json::Value response(Json::objectValue);
    response["workspace"] = retoolworkspacecodec::toJson(*workspace, true);
    response["hasExecutionContext"] = useCase_->hasExecutionContext(*workspace);
    ctl::sendJson(callback, response, k200OK);
}

void RetoolWorkspaceController::workspaceList(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto records = useCase_ ? useCase_->list() : std::vector<RetoolWorkspaceInfo>{};
    Json::Value response(Json::objectValue);
    response["items"] = Json::Value(Json::arrayValue);
    for (const auto& record : records)
    {
        Json::Value item(Json::objectValue);
        item["id"] = record.workspaceId;
        item["kind"] = "retool_workspace";
        item["provider"] = "retool";
        item["displayName"] = record.baseUrl.empty() ? record.email : record.baseUrl;
        item["status"] = record.status;
        item["metadata"] = retoolworkspacecodec::toJson(record, true);
        response["items"].append(item);
    }
    response["total"] = static_cast<Json::UInt64>(records.size());
    ctl::sendJson(callback, response, k200OK);
}

void RetoolWorkspaceController::workspacePoolStatus(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    const auto status = useCase_ ? useCase_->poolStatus() : workspace::PoolStatus{};
    Json::Value response(Json::objectValue);
    response["total"] = static_cast<Json::UInt64>(status.total);
    response["idle"] = status.idle;
    response["inUse"] = status.inUse;
    response["disabled"] = status.disabled;
    response["latestUsedAt"] = status.latestUsedAt;
    response["channel"] = status.channel
        ? channelcodec::toJson(*status.channel, true) : Json::Value(Json::objectValue);
    response["consecutiveFailures"] = status.consecutiveFailures;
    response["lastFailureAt"] = status.lastFailureAt;
    response["lastFailureReason"] = status.lastFailureReason;
    response["cooldownUntil"] = status.cooldownUntil;
    ctl::sendJson(callback, response, k200OK);
}

void RetoolWorkspaceController::workspaceDisable(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;
    const auto workspaceId = getWorkspaceId(req, jsonPtr.get());
    if (!workspaceId || workspaceId->empty())
    {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "workspaceId is required");
        return;
    }

    std::string error;
    if (!useCase_ || !useCase_->disable(*workspaceId, &error))
    {
        ctl::sendError(callback, k500InternalServerError, "workspace_disable_failed", error);
        return;
    }

    Json::Value response(Json::objectValue);
    response["status"] = "success";
    response["workspaceId"] = *workspaceId;
    response["newStatus"] = "disabled";
    ctl::sendJson(callback, response, k200OK);
}

void RetoolWorkspaceController::workspaceEnable(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;
    const auto workspaceId = getWorkspaceId(req, jsonPtr.get());
    if (!workspaceId || workspaceId->empty())
    {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "workspaceId is required");
        return;
    }

    std::string error;
    auto workspace = useCase_ ? useCase_->get(*workspaceId, &error) : std::optional<RetoolWorkspaceInfo>{};
    if (!workspace)
    {
        ctl::sendError(callback, k404NotFound, "not_found", error.empty() ? "workspace not found" : error);
        return;
    }

    std::string currentVerifyStatus;
    std::string nextStatus;
    if (!useCase_ || !useCase_->enable(
            *workspaceId, &nextStatus, &currentVerifyStatus, &error))
    {
        ctl::sendError(callback, k500InternalServerError, "workspace_enable_failed", error);
        return;
    }

    Json::Value response(Json::objectValue);
    response["status"] = "success";
    response["workspaceId"] = *workspaceId;
    response["newStatus"] = nextStatus;
    response["verifyStatus"] = currentVerifyStatus;
    ctl::sendJson(callback, response, k200OK);
}

void RetoolWorkspaceController::workspaceDelete(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;
    const auto workspaceId = getWorkspaceId(req, jsonPtr.get());
    if (!workspaceId || workspaceId->empty())
    {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "workspaceId is required");
        return;
    }

    std::string error;
    if (!useCase_ || !useCase_->remove(*workspaceId, &error))
    {
        ctl::sendError(callback, k500InternalServerError, "workspace_delete_failed", error);
        return;
    }

    Json::Value response(Json::objectValue);
    response["status"] = "success";
    response["workspaceId"] = *workspaceId;
    ctl::sendJson(callback, response, k200OK);
}

void RetoolWorkspaceController::workspaceVerify(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;
    const auto workspaceId = getWorkspaceId(req, jsonPtr.get());
    if (!workspaceId || workspaceId->empty())
    {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "workspaceId is required");
        return;
    }

    std::string error;
    auto workspace = useCase_ ? useCase_->get(*workspaceId, &error) : std::optional<RetoolWorkspaceInfo>{};
    if (!workspace)
    {
        ctl::sendError(callback, k404NotFound, "not_found", error.empty() ? "workspace not found" : error);
        return;
    }

    bool ready = false;
    std::string verifyStatus;
    RetoolWorkspaceInfo verifiedWorkspace;
    if (!useCase_ || !useCase_->verify(
            *workspaceId, &ready, &verifyStatus, &verifiedWorkspace, &error))
    {
        ctl::sendError(callback, k500InternalServerError, "workspace_verify_failed", error);
        return;
    }

    Json::Value response(Json::objectValue);
    response["workspaceId"] = *workspaceId;
    response["ready"] = ready;
    response["verifyStatus"] = verifyStatus;
    response["checks"]["baseUrl"] = !verifiedWorkspace.baseUrl.empty();
    response["checks"]["accessToken"] = !verifiedWorkspace.accessToken.empty();
    response["checks"]["xsrfToken"] = !verifiedWorkspace.xsrfToken.empty();
    response["checks"]["workflowId"] = !verifiedWorkspace.workflowId.empty();
    response["checks"]["agentId"] = !verifiedWorkspace.agentId.empty();
    ctl::sendJson(callback, response, k200OK);
}
