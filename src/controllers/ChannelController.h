#pragma once

#include <controllers/AdminAuthFilter.h>
#include <drogon/HttpController.h>
#include <domain/port/IChannelAdminUseCase.h>

/**
 * @brief 渠道管理 Controller
 *
 * 端点:
 *   GET    /aichat/channel/list           – 渠道列表
 *   POST   /aichat/channel/add            – 添加渠道
 *   POST   /aichat/channel/update         – 更新渠道
 *   POST   /aichat/channel/delete          – 删除渠道
 *   POST   /aichat/channel/update-status  – 更新渠道状态
 *
 * HTTP/JSON 适配保留在这里；跨服务的渠道写入、内置渠道保护和账号数核算
 * 由 IChannelAdminUseCase 统一编排。
 */
class ChannelController : public drogon::HttpController<ChannelController>
{
  public:
    static void setUseCase(IChannelAdminUseCase* channels);
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

  private:
    static IChannelAdminUseCase* useCase_;
};
