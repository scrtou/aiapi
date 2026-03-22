#pragma once

#include "AdminAuthFilter.h"
#include <drogon/HttpController.h>

class RetoolWorkspaceController : public drogon::HttpController<RetoolWorkspaceController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RetoolWorkspaceController::createWorkspace, "/aichat/retool/workspace/create", drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(RetoolWorkspaceController::upsertWorkspace, "/aichat/retool/workspace/upsert", drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(RetoolWorkspaceController::workspaceInfo, "/aichat/retool/workspace/info", drogon::Get, "AdminAuthFilter");
    ADD_METHOD_TO(RetoolWorkspaceController::workspaceList, "/aichat/retool/workspace/list", drogon::Get, "AdminAuthFilter");
    ADD_METHOD_TO(RetoolWorkspaceController::workspacePoolStatus, "/aichat/retool/workspace/pool-status", drogon::Get, "AdminAuthFilter");
    ADD_METHOD_TO(RetoolWorkspaceController::workspaceDisable, "/aichat/retool/workspace/disable", drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(RetoolWorkspaceController::workspaceEnable, "/aichat/retool/workspace/enable", drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(RetoolWorkspaceController::workspaceDelete, "/aichat/retool/workspace/delete", drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(RetoolWorkspaceController::workspaceVerify, "/aichat/retool/workspace/verify", drogon::Post, "AdminAuthFilter");
    METHOD_LIST_END

    void createWorkspace(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void upsertWorkspace(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void workspaceInfo(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void workspaceList(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void workspacePoolStatus(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void workspaceDisable(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void workspaceEnable(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void workspaceDelete(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void workspaceVerify(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};
