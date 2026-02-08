#pragma once

#include "AdminAuthFilter.h"
#include <drogon/HttpController.h>

/**
 * @brief 渠道管理 Controller
 *
 * 端点:
 *   GET    /aichat/channel/list           – 渠道列表
 *   POST   /aichat/channel/add            – 添加渠道
 *   POST   /aichat/channel/update         – 更新渠道
 *   POST   /aichat/channel/delete          – 删除渠道
 *   POST   /aichat/channel/update-status  – 更新渠道状态
 */
class ChannelController : public drogon::HttpController<ChannelController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ChannelController::channelList,         "/aichat/channel/list",          drogon::Get,  "AdminAuthFilter");
    ADD_METHOD_TO(ChannelController::channelAdd,          "/aichat/channel/add",           drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(ChannelController::channelUpdate,       "/aichat/channel/update",        drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(ChannelController::channelDelete,       "/aichat/channel/delete",        drogon::Post, "AdminAuthFilter");
    ADD_METHOD_TO(ChannelController::channelUpdateStatus, "/aichat/channel/update-status", drogon::Post, "AdminAuthFilter");
    METHOD_LIST_END

    void channelList(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void channelAdd(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void channelUpdate(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void channelDelete(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void channelUpdateStatus(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};
