#include <controllers/ChannelController.h>
#include <controllers/codecs/ChannelJsonCodec.h>
#include <controllers/ControllerUtils.h>

using namespace drogon;

IChannelAdminUseCase* ChannelController::useCase_ = nullptr;

void ChannelController::setUseCase(IChannelAdminUseCase* channels)
{
    useCase_ = channels;
}

namespace {

void logRecountRejection(const char* operation, const ChannelAdminResult& result)
{
    if (result.succeeded() && result.recountSubmission != TaskSubmitResult::Accepted) {
        // The channel write is already committed.  Preserve that successful
        // HTTP result while surfacing the follow-up reconciliation rejection.
        LOG_WARN << "[渠道Ctrl] " << operation << "后账号数核算任务入队被拒("
                 << toString(result.recountSubmission) << ")，渠道已写入："
                 << result.channel.channelName;
    }
}

}  // namespace

void ChannelController::channelList(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    (void)req;
    LOG_INFO << "[渠道Ctrl] 获取渠道信息";

    try {
        const auto channelList = useCase_ ? useCase_->listChannels()
                                          : std::list<Channelinfo_st>{};
        Json::Value response(Json::arrayValue);

        for (const auto &channel : channelList) {
            response.append(channelcodec::toJson(channel, true));
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
            const Channelinfo_st channel = channelcodec::fromJson(reqBody);
            const ChannelAdminResult result = useCase_ ? useCase_->add(channel)
                                                       : ChannelAdminResult{};

            Json::Value responseItem;
            responseItem["channelname"] = channel.channelName;
            if (result.outcome == ChannelAdminOutcome::ProviderRetired) {
                responseItem["status"] = "failed";
                responseItem["code"] = "provider_retired";
                responseItem["message"] = "Retired provider channels cannot be created";
            } else if (result.succeeded()) {
                responseItem["status"] = "success";
                responseItem["message"] = "Channel added successfully";
                logRecountRejection("创建渠道", result);
            } else {
                responseItem["status"] = "failed";
                responseItem["message"] = "Failed to add channel";
            }
            response.append(responseItem);
        }

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
        const Channelinfo_st channel = channelcodec::fromJson(*jsonPtr);
        const ChannelAdminResult result = useCase_ ? useCase_->update(channel)
                                                   : ChannelAdminResult{};

        if (result.outcome == ChannelAdminOutcome::ProviderRetired) {
            ctl::sendError(callback, k410Gone, "provider_retired",
                           "Retired provider channels cannot be updated");
            return;
        }
        if (result.outcome == ChannelAdminOutcome::ServiceUnavailable) {
            ctl::sendError(callback, k503ServiceUnavailable, "service_unavailable",
                           "Channel service is not available");
            return;
        }
        if (result.outcome == ChannelAdminOutcome::BuiltInChannelNotFound) {
            ctl::sendError(callback, k404NotFound, "not_found", "Built-in channel not found");
            return;
        }

        Json::Value response;
        if (result.succeeded()) {
            response["status"] = "success";
            response["message"] = "Channel updated successfully";
            response["id"] = result.channel.id;
            logRecountRejection("更新渠道", result);
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
            const int channelId = reqBody["id"].asInt();
            const ChannelAdminResult result = useCase_ ? useCase_->remove(channelId)
                                                       : ChannelAdminResult{};

            Json::Value responseItem;
            responseItem["id"] = channelId;
            if (result.succeeded()) {
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
        const std::string channelName = (*jsonPtr)["channelname"].asString();
        const bool status = (*jsonPtr)["status"].asBool();
        const ChannelAdminResult result = useCase_
            ? useCase_->updateStatus(channelName, status) : ChannelAdminResult{};

        if (result.outcome == ChannelAdminOutcome::ProviderRetired) {
            ctl::sendError(callback, k410Gone, "provider_retired",
                           "Retired provider channel status cannot be updated");
            return;
        }

        Json::Value response;
        if (result.succeeded()) {
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
