#pragma once

#include "AdminAuthFilter.h"
#include <drogon/HttpController.h>

/**
 * @brief 账号管理 Controller
 *
 * 端点:
 *   POST /aichat/account/add            – 添加账号
 *   POST /aichat/account/delete          – 删除账号
 *   POST /aichat/account/update          – 更新账号
 *   POST /aichat/account/refresh         – 刷新账号状态
 *   POST /aichat/account/autoregister    – 自动注册账号
 *   GET  /aichat/account/info            – 获取账号信息
 *   GET  /aichat/account/dbinfo          – 获取账号数据库信息
 */
class AccountController : public drogon::HttpController<AccountController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AccountController::accountAdd,          "/aichat/account/add",          drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(AccountController::accountDelete,       "/aichat/account/delete",       drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(AccountController::accountUpdate,       "/aichat/account/update",       drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(AccountController::accountRefresh,      "/aichat/account/refresh",      drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(AccountController::accountAutoRegister, "/aichat/account/autoregister", drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(AccountController::accountInfo,         "/aichat/account/info",         drogon::Get,  "AdminAuthFilter");
    ADD_METHOD_TO(AccountController::accountDbInfo,       "/aichat/account/dbinfo",       drogon::Get,  "AdminAuthFilter");
    METHOD_LIST_END

    void accountAdd(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void accountDelete(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void accountUpdate(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void accountRefresh(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void accountAutoRegister(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void accountInfo(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void accountDbInfo(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};
