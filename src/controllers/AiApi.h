#pragma once

#include <drogon/HttpController.h>
using namespace drogon;

class AiApi : public drogon::HttpController<AiApi>
{
  public:
    METHOD_LIST_BEGIN
    // use METHOD_ADD to add your custom processing function here;
    // METHOD_ADD(AiApi::get, "/{2}/{1}", Get); // path is /AiApi/{arg2}/{arg1}
    // METHOD_ADD(AiApi::your_method_name, "/{1}/{2}/list", Get); // path is /AiApi/{arg1}/{arg2}/list
    // ADD_METHOD_TO(AiApi::your_method_name, "/absolute/path/{1}/{2}/list", Get); // path is /absolute/path/{arg1}/{arg2}/list
    ADD_METHOD_TO(AiApi::chaynsapichat, "/chaynsapi/v1/chat/completions", Post); // path is /AiApi/{arg1}/{arg2}/list
    ADD_METHOD_TO(AiApi::chaynsapimodels, "/chaynsapi/v1/models", Get); // path is /AiApi/{arg1}/{arg2}/list
    ADD_METHOD_TO(AiApi::accountAdd, "/aichat/account/add", Post); // path is /AiApi/{arg1}/{arg2}/list
    ADD_METHOD_TO(AiApi::accountDelete, "/aichat/account/delete", Post); // path is /AiApi/{arg1}/{arg2}/list
    ADD_METHOD_TO(AiApi::accountInfo, "/aichat/account/info", Get); // path is /AiApi/{arg1}/{arg2}/list
    ADD_METHOD_TO(AiApi::accountDbInfo, "/aichat/account/dbinfo", Get); // path is /AiApi/{arg1}/{arg2}/list
    ADD_METHOD_TO(AiApi::channelAdd, "/aichat/channel/add", Post); // 添加渠道
    ADD_METHOD_TO(AiApi::channelInfo, "/aichat/channel/info", Get); // 获取渠道列表
    ADD_METHOD_TO(AiApi::channelDelete, "/aichat/channel/delete", Post); // 删除渠道
    ADD_METHOD_TO(AiApi::channelUpdate, "/aichat/channel/update", Post); // 更新渠道
    METHOD_LIST_END
    // your declaration of processing function maybe like this:
    // void get(const HttpRequestPtr& req, std::function<void (const HttpResponsePtr &)> &&callback, int p1, std::string p2);
    // void your_method_name(const HttpRequestPtr& req, std::function<void (const HttpResponsePtr &)> &&callback, double p1, int p2) const;
    void chaynsapichat(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
    void chaynsapimodels(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
    void accountAdd(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
    void accountDelete(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
    void accountInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
    void accountDbInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
    void channelAdd(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
    void channelInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
    void channelDelete(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
    void channelUpdate(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
  //custom function
  std::string generateClientId(const HttpRequestPtr &req);
  bool isCreateNewSession(const HttpRequestPtr &req);
};
