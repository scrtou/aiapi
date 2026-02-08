#include "ChannelController.h"
#include "ControllerUtils.h"
#include "channelManager.h"
#include "accountManager.h"
#include "BackgroundTaskQueue.h"

using namespace drogon;

void ChannelController::channelList(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[渠道Ctrl] 获取渠道信息";

    try {
        auto channelList = ChannelManager::getInstance().getChannelList();
        Json::Value response(Json::arrayValue);

        for (auto &channel : channelList) {
            response.append(channel.toJson());
        }

        ctl::sendJson(callback, response);
    } catch (const std::exception& e) {
        LOG_ERROR << "[渠道Ctrl] 获取渠道信息错误：" << e.what();
        ctl::sendError(callback, k500InternalServerError, "database_error", std::string("Database error: ") + e.what());
    }
}

void ChannelController::channelAdd(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[渠道Ctrl] 添加渠道";
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;

    try {
        Json::Value reqItems(Json::arrayValue);
        if (jsonPtr->isObject()) {
            reqItems.append(*jsonPtr);
        } else if (jsonPtr->isArray()) {
            reqItems = *jsonPtr;
        } else {
            ctl::sendError(callback, k400BadRequest, "invalid_request_error", "Request body must be a JSON object or an array of objects.");
            return;
        }

        Json::Value response(Json::arrayValue);

        for (const auto &reqBody : reqItems)
        {
            Channelinfo_st channelInfo = Channelinfo_st::fromJson(reqBody);

            Json::Value responseItem;
            responseItem["channelname"] = channelInfo.channelName;

            if (ChannelManager::getInstance().addChannel(channelInfo)) {
                responseItem["status"] = "success";
                responseItem["message"] = "Channel added successfully";
            } else {
                responseItem["status"] = "failed";
                responseItem["message"] = "Failed to add channel";
            }
            response.append(responseItem);
        }

        // 渠道添加后，异步检查渠道账号数量
        BackgroundTaskQueue::instance().enqueue("channelAdd_checkCounts", [](){
            AccountManager::getInstance().checkChannelAccountCounts();
        });

        ctl::sendJson(callback, response);
        LOG_INFO << "[渠道Ctrl] 添加渠道完成";
    } catch (const std::exception& e) {
        LOG_ERROR << "[渠道Ctrl] 添加渠道错误：" << e.what();
        ctl::sendError(callback, k500InternalServerError, "database_error", std::string("Database error: ") + e.what());
    }
}

void ChannelController::channelUpdate(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[渠道Ctrl] 更新渠道";
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;

    try {
        const auto& reqBody = *jsonPtr;
        Json::Value response;

        // 解析渠道信息
        Channelinfo_st channelInfo = Channelinfo_st::fromJson(reqBody);

        // 更新数据库
        if (ChannelManager::getInstance().updateChannel(channelInfo)) {
            response["status"] = "success";
            response["message"] = "Channel updated successfully";
            response["id"] = channelInfo.id;

            // 渠道更新后，异步检查渠道账号数量
            BackgroundTaskQueue::instance().enqueue("channelUpdate_checkCounts", [](){
                AccountManager::getInstance().checkChannelAccountCounts();
            });
        } else {
            response["status"] = "failed";
            response["message"] = "Failed to update channel";
        }

        ctl::sendJson(callback, response);
        LOG_INFO << "[渠道Ctrl] 更新渠道完成";
    } catch (const std::exception& e) {
        LOG_ERROR << "[渠道Ctrl] 更新渠道错误：" << e.what();
        ctl::sendError(callback, k500InternalServerError, "database_error", std::string("Database error: ") + e.what());
    }
}

void ChannelController::channelDelete(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[渠道Ctrl] 删除渠道";
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;

    try {
        Json::Value reqItems(Json::arrayValue);
        if (jsonPtr->isObject()) {
            reqItems.append(*jsonPtr);
        } else if (jsonPtr->isArray()) {
            reqItems = *jsonPtr;
        } else {
            ctl::sendError(callback, k400BadRequest, "invalid_request_error", "Request body must be a JSON object or an array of objects.");
            return;
        }

        Json::Value response(Json::arrayValue);

        for (const auto &reqBody : reqItems)
        {
            int channelId = reqBody["id"].asInt();

            Json::Value responseItem;
            responseItem["id"] = channelId;

            if (ChannelManager::getInstance().deleteChannel(channelId)) {
                responseItem["status"] = "success";
                responseItem["message"] = "Channel deleted successfully";
            } else {
                responseItem["status"] = "failed";
                responseItem["message"] = "Failed to delete channel";
            }
            response.append(responseItem);
        }

        ctl::sendJson(callback, response);
    } catch (const std::exception& e) {
        LOG_ERROR << "[渠道Ctrl] 删除渠道错误：" << e.what();
        ctl::sendError(callback, k500InternalServerError, "database_error", std::string("Database error: ") + e.what());
    }
}

void ChannelController::channelUpdateStatus(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[渠道Ctrl] 更新渠道状态";
    std::shared_ptr<Json::Value> jsonPtr;
    if (!ctl::parseJsonOrError(req, callback, jsonPtr)) return;

    try {
        std::string channelName = (*jsonPtr)["channelname"].asString();
        bool status = (*jsonPtr)["status"].asBool();

        Json::Value response;
        if (ChannelManager::getInstance().updateChannelStatus(channelName, status)) {
            response["status"] = "success";
            response["message"] = "Channel status updated successfully";
        } else {
            response["status"] = "failed";
            response["message"] = "Failed to update channel status";
        }

        ctl::sendJson(callback, response);
    } catch (const std::exception& e) {
        LOG_ERROR << "[渠道Ctrl] 更新渠道状态错误：" << e.what();
        ctl::sendError(callback, k500InternalServerError, "internal_error", std::string("Error: ") + e.what());
    }
}
