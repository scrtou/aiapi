#include "RetoolWorkspaceController.h"

#include "ControllerUtils.h"
#include <channelManager/channelManager.h>
#include <dbManager/config/ConfigDbManager.h>
#include <managedAccount/service/ManagedAccountService.h>
#include <optional>
#include <retoolWorkspace/RetoolWorkspaceInfo.h>
#include <retoolWorkspace/RetoolWorkspaceManager.h>
#include <retoolWorkspace/RetoolWorkspaceService.h>

using namespace drogon;

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

std::optional<std::string> getConfigValue(const std::string& key)
{
    return ConfigDbManager::getInstance()->getValue(key, nullptr);
}

void mergeWorkspaceInfoPreservingExisting(
    RetoolWorkspaceInfo& incoming,
    const RetoolWorkspaceInfo& existing)
{
    auto keepIfEmpty = [](std::string& target, const std::string& fallback) {
        if (target.empty()) target = fallback;
    };

    keepIfEmpty(incoming.email, existing.email);
    keepIfEmpty(incoming.password, existing.password);
    keepIfEmpty(incoming.mailProvider, existing.mailProvider);
    keepIfEmpty(incoming.mailAccountId, existing.mailAccountId);
    keepIfEmpty(incoming.baseUrl, existing.baseUrl);
    keepIfEmpty(incoming.subdomain, existing.subdomain);
    keepIfEmpty(incoming.accessToken, existing.accessToken);
    keepIfEmpty(incoming.xsrfToken, existing.xsrfToken);
    keepIfEmpty(incoming.extraCookiesJson, existing.extraCookiesJson);
    keepIfEmpty(incoming.openaiResourceUuid, existing.openaiResourceUuid);
    keepIfEmpty(incoming.openaiResourceName, existing.openaiResourceName);
    keepIfEmpty(incoming.anthropicResourceUuid, existing.anthropicResourceUuid);
    keepIfEmpty(incoming.anthropicResourceName, existing.anthropicResourceName);
    keepIfEmpty(incoming.workflowId, existing.workflowId);
    keepIfEmpty(incoming.workflowApiKey, existing.workflowApiKey);
    keepIfEmpty(incoming.agentId, existing.agentId);
    keepIfEmpty(incoming.status, existing.status);
    keepIfEmpty(incoming.verifyStatus, existing.verifyStatus);
    keepIfEmpty(incoming.lastVerifyAt, existing.lastVerifyAt);
    keepIfEmpty(incoming.lastUsedAt, existing.lastUsedAt);
    keepIfEmpty(incoming.notesJson, existing.notesJson);
    keepIfEmpty(incoming.createdAt, existing.createdAt);
    keepIfEmpty(incoming.updatedAt, existing.updatedAt);

    if (incoming.inUseCount == 0 && existing.inUseCount != 0)
    {
        incoming.inUseCount = existing.inUseCount;
    }
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
        auto workspace = RetoolWorkspaceService::getInstance().provisionWorkspace(*jsonPtr);
        Json::Value response(Json::objectValue);
        response["status"] = "success";
        response["workspace"] = workspace.toJson(false);
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

    RetoolWorkspaceInfo info = RetoolWorkspaceInfo::fromJson(*jsonPtr);
    if (info.workspaceId.empty())
    {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "workspaceId is required");
        return;
    }

    std::string error;
    if (auto existing = RetoolWorkspaceManager::getInstance().getWorkspace(info.workspaceId, &error); existing)
    {
        mergeWorkspaceInfoPreservingExisting(info, *existing);
    }
    else
    {
        error.clear();
    }

    if (info.baseUrl.empty())
    {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "baseUrl is required");
        return;
    }

    if (!RetoolWorkspaceManager::getInstance().upsertWorkspace(info, &error))
    {
        ctl::sendError(callback, k500InternalServerError, "workspace_upsert_failed", error);
        return;
    }

    Json::Value response(Json::objectValue);
    response["status"] = "success";
    response["workspace"] = info.toJson(false);
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
    auto workspace = RetoolWorkspaceManager::getInstance().getWorkspace(*workspaceId, &error);
    if (!workspace)
    {
        ctl::sendError(callback, k404NotFound, "not_found", error.empty() ? "workspace not found" : error);
        return;
    }

    Json::Value response(Json::objectValue);
    response["workspace"] = workspace->toJson(false);
    auto executionContext = ManagedAccountService::getInstance().buildExecutionContext(
        ManagedAccountKind::RetoolWorkspace, *workspaceId, nullptr);
    response["hasExecutionContext"] = executionContext.has_value();
    ctl::sendJson(callback, response, k200OK);
}

void RetoolWorkspaceController::workspaceList(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto records = ManagedAccountService::getInstance().listByKind(ManagedAccountKind::RetoolWorkspace);
    Json::Value response(Json::objectValue);
    response["items"] = Json::Value(Json::arrayValue);
    for (const auto& record : records)
    {
        response["items"].append(record.toJson());
    }
    response["total"] = static_cast<Json::UInt64>(records.size());
    ctl::sendJson(callback, response, k200OK);
}

void RetoolWorkspaceController::workspacePoolStatus(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto workspaces = RetoolWorkspaceManager::getInstance().listWorkspaces();
    Json::Value response(Json::objectValue);
    int idle = 0;
    int inUse = 0;
    int disabled = 0;
    std::string latestUsedAt;
    for (const auto& workspace : workspaces)
    {
        if (!workspace.lastUsedAt.empty() && (latestUsedAt.empty() || workspace.lastUsedAt > latestUsedAt))
        {
            latestUsedAt = workspace.lastUsedAt;
        }
        if (workspace.status == "disabled")
        {
            ++disabled;
        }
        else if (workspace.inUseCount > 0)
        {
            ++inUse;
        }
        else
        {
            ++idle;
        }
    }

    Json::Value channelJson(Json::objectValue);
    for (const auto& channel : ChannelManager::getInstance().getChannelList())
    {
        if (channel.channelName == "retoolapi")
        {
            channelJson = channel.toJson();
            break;
        }
    }

    response["total"] = static_cast<Json::UInt64>(workspaces.size());
    response["idle"] = idle;
    response["inUse"] = inUse;
    response["disabled"] = disabled;
    response["latestUsedAt"] = latestUsedAt;
    response["channel"] = channelJson;
    int consecutiveFailures = 0;
    if (const auto value = getConfigValue("retoolapi.provision.consecutive_failures"); value && !value->empty())
    {
        try { consecutiveFailures = std::stoi(*value); } catch (...) {}
    }
    response["consecutiveFailures"] = consecutiveFailures;
    response["lastFailureAt"] = getConfigValue("retoolapi.provision.last_failure_at").value_or("");
    response["lastFailureReason"] = getConfigValue("retoolapi.provision.last_failure_reason").value_or("");
    response["cooldownUntil"] = getConfigValue("retoolapi.provision.cooldown_until").value_or("");
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
    if (!ManagedAccountService::getInstance().disable(ManagedAccountKind::RetoolWorkspace, *workspaceId, &error))
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
    auto workspace = RetoolWorkspaceManager::getInstance().getWorkspace(*workspaceId, &error);
    if (!workspace)
    {
        ctl::sendError(callback, k404NotFound, "not_found", error.empty() ? "workspace not found" : error);
        return;
    }

    const auto currentVerifyStatus = workspace->verifyStatus.empty() ? std::string("unknown") : workspace->verifyStatus;
    const auto nextStatus =
        (currentVerifyStatus == "ready" || currentVerifyStatus == "passed")
            ? std::string("ready")
            : std::string("needs_attention");

    if (!RetoolWorkspaceManager::getInstance().updateWorkspaceStatus(
            *workspaceId,
            nextStatus,
            currentVerifyStatus,
            &error))
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
    if (!RetoolWorkspaceManager::getInstance().deleteWorkspace(*workspaceId, &error))
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
    auto workspace = RetoolWorkspaceManager::getInstance().getWorkspace(*workspaceId, &error);
    if (!workspace)
    {
        ctl::sendError(callback, k404NotFound, "not_found", error.empty() ? "workspace not found" : error);
        return;
    }

    const bool ready = !workspace->baseUrl.empty() &&
                       !workspace->accessToken.empty() &&
                       !workspace->xsrfToken.empty() &&
                       !workspace->workflowId.empty() &&
                       !workspace->agentId.empty();
    const std::string verifyStatus = ready ? "ready" : "incomplete";

    if (!RetoolWorkspaceManager::getInstance().updateWorkspaceStatus(
            *workspaceId,
            ready ? workspace->status : "needs_attention",
            verifyStatus,
            &error))
    {
        ctl::sendError(callback, k500InternalServerError, "workspace_verify_failed", error);
        return;
    }

    Json::Value response(Json::objectValue);
    response["workspaceId"] = *workspaceId;
    response["ready"] = ready;
    response["verifyStatus"] = verifyStatus;
    response["checks"]["baseUrl"] = !workspace->baseUrl.empty();
    response["checks"]["accessToken"] = !workspace->accessToken.empty();
    response["checks"]["xsrfToken"] = !workspace->xsrfToken.empty();
    response["checks"]["workflowId"] = !workspace->workflowId.empty();
    response["checks"]["agentId"] = !workspace->agentId.empty();
    ctl::sendJson(callback, response, k200OK);
}
