#include "AiApi.h"
#include <drogon/HttpResponse.h>
#include <json/json.h>
#include <drogon/drogon.h>
#include <unistd.h>
#include <random>
#include <unordered_map>
#include <apiManager/ApiManager.h>
#include <accountManager/accountManager.h>
#include <dbManager/account/accountDbManager.h>
#include <dbManager/metrics/ErrorStatsDbManager.h>
#include <dbManager/metrics/StatusDbManager.h>
#include <channelManager/channelManager.h>
#include <sessionManager/Session.h>
#include <sessionManager/ClientOutputSanitizer.h>
#include <sessionManager/GenerationService.h>
#include <sessionManager/GenerationRequest.h>
#include <sessionManager/IResponseSink.h>
#include <sessionManager/SessionExecutionGate.h>
#include <sessionManager/Errors.h>
#include <sessionManager/RequestAdapters.h>
#include <sessionManager/ResponseIndex.h>
#include <controllers/sinks/ChatSseSink.h>
#include <controllers/sinks/ChatJsonSink.h>
#include <controllers/sinks/ResponsesSseSink.h>
#include <controllers/sinks/ResponsesJsonSink.h>
#include <drogon/orm/Exception.h>
#include <drogon/orm/DbClient.h>
#include <vector> // 添加 vector 头文件
#include <ctime>
#include <optional>
#include <iomanip>
#include <sstream>
#include <cstring>
#include "AiApi.h"
#include <fstream>      // 处理文件流
#include <filesystem>   // 处理目录和文件信息
#include <vector>       // 处理容器
#include <string>       // 处理字符串
#include <chrono>       // 处理系统时间
#include <iomanip>      // 如果用到了格式化输出
#include <algorithm>    // 处理 std::min
using namespace drogon;
using namespace drogon::orm;

// Add definition of your processing function here
// 响应存储已迁移到 Session 层统一管理
void AiApi::chaynsapichat(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    //打印所有的请求头
    LOG_INFO<<"**************接收到chaynsapichat请求******************";
    LOG_DEBUG<<"请求头:";
    for(auto &header : req->getHeaders())
    {
        LOG_DEBUG<<header.first<<":"<<header.second;
    }
    //打印请求信息
    LOG_DEBUG<<"请求信息:"<<req->getBody();

    auto jsonPtr = req->getJsonObject();
       if (!jsonPtr) {
        Json::Value error;
        error["error"]["message"] = "Invalid JSON in request body";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }
    auto& reqbody = *jsonPtr;

    auto& reqmessages = reqbody["messages"];
    if (reqmessages.empty()) {
        Json::Value error;
        error["error"]["message"] = "Messages array cannot be empty";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }
    
    const bool stream = reqbody["stream"].asBool();
    LOG_INFO << "[API接口] 流式模式: " << stream;
    
    // ========== PR4: 使用 RequestAdapters 构建 GenerationRequest ==========
    LOG_INFO << "[API接口] 使用 RequestAdapters 构建 GenerationRequest";
    GenerationRequest genReq = RequestAdapters::buildGenerationRequestFromChat(req);
    
    // 设置默认 API（如果为空）
    if (genReq.provider.empty()) {
        genReq.provider = "chaynsapi";
    }
    
    // ========== 非流式请求：使用 GenerationService::runGuarded() ==========
    if (!stream) {
        LOG_INFO << "非流式响应 - 使用 GenerationService::runGuarded()";
        
        HttpResponsePtr jsonResp;
        int httpStatus = 200;
        
        // 创建 ChatJsonSink
        ChatJsonSink jsonSink(
            [&jsonResp, &httpStatus](const Json::Value& response, int status) {
                jsonResp = HttpResponse::newHttpJsonResponse(response);
                httpStatus = status;
            },
            genReq.model
        );
        
        // 使用 GenerationService::runGuarded() 执行生成
        GenerationService genService;
        auto err = genService.runGuarded(
            genReq, jsonSink,
            session::ConcurrencyPolicy::RejectConcurrent
        );
        
        // 检查是否有并发冲突错误
        if (err.has_value()) {
            Json::Value errorJson;
            errorJson["error"]["message"] = err->message;
            errorJson["error"]["type"] = err->type();
            auto errorResp = HttpResponse::newHttpJsonResponse(errorJson);
            errorResp->setStatusCode(static_cast<HttpStatusCode>(err->httpStatus()));
            callback(errorResp);
            return;
        }
        
        // 设置状态码并返回响应
        if (jsonResp) {
            jsonResp->setStatusCode(static_cast<HttpStatusCode>(httpStatus));
            jsonResp->setContentTypeString("application/json; charset=utf-8");
            callback(jsonResp);
        } else {
            // 备用响应
            Json::Value error;
            error["error"]["message"] = "Failed to generate response";
            error["error"]["type"] = "internal_error";
            auto errorResp = HttpResponse::newHttpJsonResponse(error);
            errorResp->setStatusCode(k500InternalServerError);
            callback(errorResp);
        }
        return;
    }
    
    // ========== 流式请求：使用 GenerationService::runGuarded() ==========
    LOG_INFO << "[API接口] 流式响应 - 使用 GenerationService::runGuarded()";
    LOG_INFO << "[API接口] previousResponseId: "
             << (genReq.previousResponseId.has_value() ? *genReq.previousResponseId : "");
    
    // 使用 CollectorSink 收集完整响应
    CollectorSink collector;
    GenerationService genService;
    auto gateErr = genService.runGuarded(
        genReq, collector,
        session::ConcurrencyPolicy::RejectConcurrent
    );
    
    // 检查是否有并发冲突错误
    if (gateErr.has_value()) {
        Json::Value errorJson;
        errorJson["error"]["message"] = gateErr->message;
        errorJson["error"]["type"] = gateErr->type();
        auto errorResp = HttpResponse::newHttpJsonResponse(errorJson);
        errorResp->setStatusCode(static_cast<HttpStatusCode>(gateErr->httpStatus()));
        callback(errorResp);
        return;
    }
    
    // Stream via ChatSseSink so tool_calls can be forwarded correctly.
    if (collector.hasError()) {
        auto error = collector.getError();
        Json::Value errorJson;
        errorJson["error"]["message"] = error ? error->message : "Provider error";
        errorJson["error"]["type"] = error ? generation::errorCodeToString(error->code) : "provider_error";
        errorJson["error"]["code"] = errorJson["error"]["type"];
        auto errorResp = HttpResponse::newHttpJsonResponse(errorJson);
        errorResp->setStatusCode(static_cast<HttpStatusCode>(
            error ? generation::errorCodeToHttpStatus(error->code) : 500
        ));
        errorResp->setContentTypeString("application/json; charset=utf-8");
        callback(errorResp);
        return;
    }

    std::string ssePayload;
    ChatSseSink sseSink(
        [&ssePayload](const std::string& chunk) {
            ssePayload += chunk;
            return true;
        },
        []() {},
        genReq.model
    );

    for (const auto& ev : collector.getEvents()) {
        sseSink.onEvent(ev);
    }
    sseSink.onClose();

    struct StreamContext {
        size_t pos = 0;
        time_t start_time = time(nullptr);
        explicit StreamContext() = default;
    };

    auto shared_payload = std::make_shared<std::string>(std::move(ssePayload));
    auto shared_context = std::make_shared<StreamContext>();

    auto resp = HttpResponse::newStreamResponse(
        [shared_payload, shared_context](char *buffer, size_t maxBytes) -> size_t {
            try {
                if (time(nullptr) - shared_context->start_time > 60) {
                    LOG_WARN << "[API接口] 流式传输超时";
                    return 0;
                }

                if (shared_context->pos >= shared_payload->size()) {
                    return 0;
                }

                size_t remaining = shared_payload->size() - shared_context->pos;
                size_t to_send = std::min(remaining, maxBytes);
                memcpy(buffer, shared_payload->data() + shared_context->pos, to_send);
                shared_context->pos += to_send;
                return to_send;
            } catch (const std::exception& e) {
                LOG_ERROR << "[API接口] 流式响应错误: " << e.what();
                return 0;
            }
        }
    );

    resp->setContentTypeString("text/event-stream; charset=utf-8");
    resp->addHeader("Cache-Control", "no-cache");
    resp->addHeader("Connection", "keep-alive");
    resp->addHeader("X-Accel-Buffering", "no");
    resp->addHeader("Keep-Alive", "timeout=60");
    callback(resp);
    LOG_INFO << "流式响应发送完成";
}
void AiApi::chaynsapimodels(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 获取模型列表";
    Json::Value response= ApiManager::getInstance().getApiByApiName("chaynsapi")->getModels();
    auto resp = HttpResponse::newHttpJsonResponse(response);
    callback(resp);
}
void AiApi::accountAdd(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 添加账号";
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        Json::Value error;
        error["error"]["message"] = "Invalid JSON in request body";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }

    Json::Value reqItems(Json::arrayValue);
    if (jsonPtr->isObject()) {
        reqItems.append(*jsonPtr);
    } else if (jsonPtr->isArray()) {
        reqItems = *jsonPtr;
    } else {
        Json::Value error;
        error["error"]["message"] = "Request body must be a JSON object or an array of objects.";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }

    LOG_INFO << "[API接口] 开始添加账号";
    Json::Value response;
    list<Accountinfo_st> accountList;
    
    // 生成当前时间字符串
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_time_t), "%Y-%m-%d %H:%M:%S");
    std::string currentTime = ss.str();
    
    for(auto &item:reqItems)
    {   Accountinfo_st accountinfo;
        accountinfo.apiName=item["apiname"].asString();
        accountinfo.userName=item["username"].asString();
        accountinfo.passwd=item["password"].asString();
        accountinfo.authToken=item["authtoken"].empty()?"":item["authtoken"].asString();
        accountinfo.userTobitId=item["usertobitid"].empty()?0:item["usertobitid"].asInt();
        accountinfo.personId=item["personid"].empty()?"":item["personid"].asString();
        accountinfo.useCount=item["usecount"].empty()?0:item["usecount"].asInt();
        accountinfo.tokenStatus=item["tokenstatus"].empty()?false:item["tokenstatus"].asBool();
        accountinfo.accountStatus=item["accountstatus"].empty()?false:item["accountstatus"].asBool();
        accountinfo.accountType=item["accounttype"].empty()?"free":item["accounttype"].asString();
        accountinfo.createTime=currentTime;
        Json::Value responseitem;
        responseitem["apiname"]=accountinfo.apiName;
        responseitem["username"]=accountinfo.userName;
        //先添加到accountManager
        if(AccountManager::getInstance().addAccountbyPost(accountinfo))
        {
            responseitem["status"]="success";
            accountList.push_back(accountinfo);
        }
        else
        {
            responseitem["status"]="failed";
        }
        response.append(responseitem);
    }
    thread addAccountThread([accountList](){
        for(auto &account:accountList)
        {
            AccountDbManager::getInstance()->addAccount(account);
        }
        AccountManager::getInstance().checkUpdateAccountToken();
        // 账号添加后，只对新添加的账号更新 accountType
        for(const auto &account : accountList)
        {
            auto accountMap = AccountManager::getInstance().getAccountList();
            if (accountMap.find(account.apiName) != accountMap.end() &&
                accountMap[account.apiName].find(account.userName) != accountMap[account.apiName].end()) {
                AccountManager::getInstance().updateAccountType(accountMap[account.apiName][account.userName]);
            }
        }
    });
    addAccountThread.detach();
    auto resp = HttpResponse::newHttpJsonResponse(response);
    callback(resp);
    LOG_INFO << "[API接口] 添加账号完成";
}
void AiApi::accountInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 获取账号信息";
    auto accountList=AccountManager::getInstance().getAccountList();
    Json::Value response(Json::arrayValue);
    for(auto &account:accountList)
    {
        for(auto &userName:account.second)
        {
            Json::Value accountitem;
            accountitem["apiname"]=userName.second->apiName;
            accountitem["username"]=userName.second->userName;
            accountitem["password"]=userName.second->passwd;
            accountitem["authtoken"]=userName.second->authToken;
            accountitem["usecount"]=userName.second->useCount;
            accountitem["tokenstatus"]=userName.second->tokenStatus;
            accountitem["accountstatus"]=userName.second->accountStatus;
            accountitem["usertobitid"]=userName.second->userTobitId;
            accountitem["personid"]=userName.second->personId;
            accountitem["createtime"]=userName.second->createTime;
            accountitem["accounttype"]=userName.second->accountType;
            response.append(accountitem);
        }
    }
    if (response.empty()) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k200OK);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody("[]");
        callback(resp);
    } else {
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
    }
}
void AiApi::accountDelete(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 删除账号";
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        Json::Value error;
        error["error"]["message"] = "Invalid JSON in request body";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }

    Json::Value reqItems(Json::arrayValue);
    if (jsonPtr->isObject()) {
        reqItems.append(*jsonPtr);
    } else if (jsonPtr->isArray()) {
        reqItems = *jsonPtr;
    } else {
        Json::Value error;
        error["error"]["message"] = "Request body must be a JSON object or an array of objects.";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }
 
    Json::Value response;
    list<Accountinfo_st> accountList;
    
    // 在删除前获取完整账号信息（包含 authToken、passwd 等），用于上游删除
    auto currentAccountMap = AccountManager::getInstance().getAccountList();
    
    for(auto &item:reqItems)
    {
        Accountinfo_st accountinfo;
        Json::Value responseitem;
        accountinfo.apiName=item["apiname"].asString();
        accountinfo.userName=item["username"].asString();
        responseitem["apiname"]=accountinfo.apiName;
        responseitem["username"]=accountinfo.userName;

        // 检查账号是否正在注册中，如果是则拒绝删除
        if (AccountManager::getInstance().isAccountRegisteringByUsername(accountinfo.userName)) {
            responseitem["status"] = "failed";
            responseitem["error"] = "Account is currently being registered, cannot delete";
            LOG_WARN << "[API接口] 账号 " << accountinfo.userName << " 正在注册中，无法删除";
            response.append(responseitem);
            continue;
        }

        // 在从内存删除前，获取完整的账号信息用于上游删除
        if (currentAccountMap.find(accountinfo.apiName) != currentAccountMap.end() &&
            currentAccountMap[accountinfo.apiName].find(accountinfo.userName) != currentAccountMap[accountinfo.apiName].end()) {
            auto fullAccount = currentAccountMap[accountinfo.apiName][accountinfo.userName];
            accountinfo.passwd = fullAccount->passwd;
            accountinfo.authToken = fullAccount->authToken;
            accountinfo.userTobitId = fullAccount->userTobitId;
            accountinfo.personId = fullAccount->personId;
        }

        if(AccountManager::getInstance().deleteAccountbyPost(accountinfo.apiName,accountinfo.userName))
        {
            responseitem["status"]="success";
            accountList.push_back(accountinfo);
        }
        else
        {
            responseitem["status"]="failed";
        }
        response.append(responseitem);
    }
    thread deleteAccountThread([accountList](){
        for(auto &account:accountList)
        {
            // 先从上游删除账号
            bool upstreamDeleted = AccountManager::getInstance().deleteUpstreamAccount(account);
            if (upstreamDeleted) {
                LOG_INFO << "[API接口] 上游账号删除成功: " << account.userName;
            } else {
                LOG_WARN << "[API接口] 上游账号删除失败（继续删除本地数据库）: " << account.userName;
            }
            // 再从本地数据库删除
            AccountDbManager::getInstance()->deleteAccount(account.apiName,account.userName);
        }
        AccountManager::getInstance().loadAccount();
        // 账号删除后，检查渠道账号数量（可能需要补充账号）
        AccountManager::getInstance().checkChannelAccountCounts();
    });
    deleteAccountThread.detach();
    auto resp = HttpResponse::newHttpJsonResponse(response);
    callback(resp);
    //删除accountdbManager中的账号
}
void AiApi::accountDbInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 获取账号数据库信息";
    Json::Value response;
    response["dbName"]="aichat";
    response["tableName"]="account";
    for(auto &account:AccountDbManager::getInstance()->getAccountDBList())
    {
        Json::Value accountitem;
        accountitem["apiname"]=account.apiName;
        accountitem["username"]=account.userName;
        accountitem["password"]=account.passwd;
        accountitem["authtoken"]=account.authToken;
        accountitem["usecount"]=account.useCount;
        accountitem["tokenstatus"]=account.tokenStatus;
        accountitem["accountstatus"]=account.accountStatus;
        accountitem["usertobitid"]=account.userTobitId;
        accountitem["personid"]=account.personId;
        accountitem["createtime"]=account.createTime;
        accountitem["accounttype"]=account.accountType;
        response.append(accountitem);
    }
    auto resp = HttpResponse::newHttpJsonResponse(response);
    callback(resp);
}

// 渠道管理接口实现
void AiApi::channelAdd(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 添加渠道";
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        Json::Value error;
        error["error"]["message"] = "Invalid JSON in request body";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }

    try {
        Json::Value response;
        
        for (auto &reqBody : *jsonPtr)
        {
            Channelinfo_st channelInfo;
            channelInfo.channelName = reqBody["channelname"].asString();
            channelInfo.channelType = reqBody["channeltype"].asString();
            channelInfo.channelUrl = reqBody["channelurl"].asString();
            channelInfo.channelKey = reqBody["channelkey"].asString();
            channelInfo.channelStatus = reqBody["channelstatus"].empty() ? true : reqBody["channelstatus"].asBool();
            channelInfo.maxConcurrent = reqBody["maxconcurrent"].empty() ? 10 : reqBody["maxconcurrent"].asInt();
            channelInfo.timeout = reqBody["timeout"].empty() ? 30 : reqBody["timeout"].asInt();
            channelInfo.priority = reqBody["priority"].empty() ? 0 : reqBody["priority"].asInt();
            channelInfo.description = reqBody["description"].empty() ? "" : reqBody["description"].asString();
            channelInfo.accountCount = reqBody["accountcount"].empty() ? 0 : reqBody["accountcount"].asInt();
            channelInfo.supportsToolCalls = reqBody["supports_tool_calls"].empty() ? false : reqBody["supports_tool_calls"].asBool();
            
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
        std::thread([](){
            AccountManager::getInstance().checkChannelAccountCounts();
        }).detach();
        
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
        LOG_INFO << "[API接口] 添加渠道完成";
    } catch (const std::exception& e) {
        LOG_ERROR << "[API接口] 添加渠道错误: " << e.what();
        Json::Value error;
        error["error"]["message"] = std::string("Database error: ") + e.what();
        error["error"]["type"] = "database_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k500InternalServerError);
        callback(resp);
    }
}

void AiApi::channelInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 获取渠道信息";
    
    try {
        auto channelList = ChannelManager::getInstance().getChannelList();
        Json::Value response(Json::arrayValue);
        
        for (auto &channel : channelList) {
            Json::Value channelItem;
            channelItem["id"] = channel.id;
            channelItem["channelname"] = channel.channelName;
            channelItem["channeltype"] = channel.channelType;
            channelItem["channelurl"] = channel.channelUrl;
            channelItem["channelkey"] = channel.channelKey;
            channelItem["channelstatus"] = channel.channelStatus;
            channelItem["maxconcurrent"] = channel.maxConcurrent;
            channelItem["timeout"] = channel.timeout;
            channelItem["priority"] = channel.priority;
            channelItem["description"] = channel.description;
            channelItem["createtime"] = channel.createTime;
            channelItem["updatetime"] = channel.updateTime;
            channelItem["accountcount"] = channel.accountCount;
            channelItem["supports_tool_calls"] = channel.supportsToolCalls;
            response.append(channelItem);
        }
        
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
    } catch (const std::exception& e) {
        LOG_ERROR << "[API接口] 获取渠道信息错误: " << e.what();
        Json::Value error;
        error["error"]["message"] = std::string("Database error: ") + e.what();
        error["error"]["type"] = "database_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k500InternalServerError);
        callback(resp);
    }
}

void AiApi::channelDelete(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 删除渠道";
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        Json::Value error;
        error["error"]["message"] = "Invalid JSON in request body";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }
    
    try {
        Json::Value response;
        
        for (auto &reqBody : *jsonPtr)
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
        
        // 渠道删除后不需要更新 accountType
        
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
    } catch (const std::exception& e) {
        LOG_ERROR << "[API接口] 删除渠道错误: " << e.what();
        Json::Value error;
        error["error"]["message"] = std::string("Database error: ") + e.what();
        error["error"]["type"] = "database_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k500InternalServerError);
        callback(resp);
    }
}

void AiApi::channelUpdate(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 更新渠道";
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        Json::Value error;
        error["error"]["message"] = "Invalid JSON in request body";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }

try {
        auto& reqBody = *jsonPtr;
        Json::Value response;

// 解析渠道信息
        Channelinfo_st channelInfo;
        channelInfo.id = reqBody["id"].asInt();
        channelInfo.channelName = reqBody["channelname"].asString();
        channelInfo.channelType = reqBody["channeltype"].asString();
        channelInfo.channelUrl = reqBody["channelurl"].asString();
        channelInfo.channelKey = reqBody["channelkey"].asString();
        channelInfo.channelStatus = reqBody["channelstatus"].asBool();
        channelInfo.maxConcurrent = reqBody["maxconcurrent"].asInt();
        channelInfo.timeout = reqBody["timeout"].asInt();
        channelInfo.priority = reqBody["priority"].asInt();
        channelInfo.description = reqBody["description"].empty() ? "" : reqBody["description"].asString();
        channelInfo.accountCount = reqBody["accountcount"].asInt();
        channelInfo.supportsToolCalls = reqBody["supports_tool_calls"].empty() ? false : reqBody["supports_tool_calls"].asBool();

// 更新数据库
        if (ChannelManager::getInstance().updateChannel(channelInfo)) {
            response["status"] = "success";
            response["message"] = "Channel updated successfully";
            response["id"] = channelInfo.id;
            
            // 渠道更新后，异步检查渠道账号数量
            std::thread([](){
                AccountManager::getInstance().checkChannelAccountCounts();
            }).detach();
        } else {
            response["status"] = "failed";
            response["message"] = "Failed to update channel";
        }

auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
        LOG_INFO << "[API接口] 更新渠道完成";
    } catch (const std::exception& e) {
        LOG_ERROR << "[API接口] 更新渠道错误: " << e.what();
        Json::Value error;
        error["error"]["message"] = std::string("Database error: ") + e.what();
        error["error"]["type"] = "database_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k500InternalServerError);
        callback(resp);
    }
}

void AiApi::accountUpdate(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 更新账号";
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        Json::Value error;
        error["error"]["message"] = "Invalid JSON in request body";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }

    Json::Value reqItems(Json::arrayValue);
    if (jsonPtr->isObject()) {
        reqItems.append(*jsonPtr);
    } else if (jsonPtr->isArray()) {
        reqItems = *jsonPtr;
    } else {
        Json::Value error;
        error["error"]["message"] = "Request body must be a JSON object or an array of objects.";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }

    Json::Value response;
    list<Accountinfo_st> accountList;
    
    for(auto &item:reqItems)
    {   
        Accountinfo_st accountinfo;
        accountinfo.apiName=item["apiname"].asString();
        accountinfo.userName=item["username"].asString();
        accountinfo.passwd=item["password"].asString();
        accountinfo.authToken=item["authtoken"].empty()?"":item["authtoken"].asString();
        accountinfo.userTobitId=item["usertobitid"].empty()?0:item["usertobitid"].asInt();
        accountinfo.personId=item["personid"].empty()?"":item["personid"].asString();
        accountinfo.useCount=item["usecount"].empty()?0:item["usecount"].asInt();
        accountinfo.tokenStatus=item["tokenstatus"].empty()?false:item["tokenstatus"].asBool();
        accountinfo.accountStatus=item["accountstatus"].empty()?false:item["accountstatus"].asBool();
        accountinfo.accountType=item["accounttype"].empty()?"free":item["accounttype"].asString();

        Json::Value responseitem;
        responseitem["apiname"]=accountinfo.apiName;
        responseitem["username"]=accountinfo.userName;
        
        if(AccountManager::getInstance().updateAccount(accountinfo))
        {
            responseitem["status"]="success";
            accountList.push_back(accountinfo);
        }
        else
        {
            responseitem["status"]="failed";
            responseitem["message"]="Account not found";
        }
        response.append(responseitem);
    }

    thread updateAccountThread([accountList](){
        for(auto &account:accountList)
        {
            AccountDbManager::getInstance()->updateAccount(account);
        }
        // 账号更新后，只对操作的账号更新 accountType
        for(const auto &account : accountList)
        {
            auto accountMap = AccountManager::getInstance().getAccountList();
            if (accountMap.find(account.apiName) != accountMap.end() &&
                accountMap[account.apiName].find(account.userName) != accountMap[account.apiName].end()) {
                AccountManager::getInstance().updateAccountType(accountMap[account.apiName][account.userName]);
            }
        }
    });
    updateAccountThread.detach();

    auto resp = HttpResponse::newHttpJsonResponse(response);
    callback(resp);
    LOG_INFO << "[API接口] 更新账号完成";
}

void AiApi::accountRefresh(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 刷新账号状态（token有效性+账号类型）";

    Json::Value response;
    response["status"] = "started";
    response["message"] = "Account status refresh started in background";

    // 异步执行刷新操作
    std::thread([](){
        LOG_INFO << "[API接口] 后台刷新：开始检查 token 有效性";
        AccountManager::getInstance().checkToken();
        LOG_INFO << "[API接口] 后台刷新：开始更新账号类型";
        AccountManager::getInstance().updateAllAccountTypes();
        LOG_INFO << "[API接口] 后台刷新：刷新完成";
    }).detach();

    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k200OK);
    callback(resp);
}

void AiApi::accountAutoRegister(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 自动注册账号";
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        Json::Value error;
        error["error"]["message"] = "Invalid JSON in request body";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }

    auto& reqBody = *jsonPtr;
    std::string apiName = reqBody.get("apiname", "chaynsapi").asString();
    int count = reqBody.get("count", 1).asInt();

    // 限制一次最多注册 20 个
    if (count < 1) count = 1;
    if (count > 20) count = 20;

    LOG_INFO << "[API接口] 自动注册: apiName=" << apiName << ", count=" << count;

    Json::Value response;
    response["status"] = "started";
    response["message"] = "Auto registration started in background";
    response["apiname"] = apiName;
    response["count"] = count;

    // 异步执行注册操作
    std::thread([apiName, count](){
        LOG_INFO << "[API接口] 后台注册：开始为 " << apiName << " 注册 " << count << " 个账号";
        for (int i = 0; i < count; ++i) {
            LOG_INFO << "[API接口] 后台注册：正在注册第 " << (i + 1) << "/" << count << " 个账号";
            AccountManager::getInstance().autoRegisterAccount(apiName);
            // 注册间隔 5 秒，避免过快
            if (i < count - 1) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
        LOG_INFO << "[API接口] 后台注册：注册完成";
    }).detach();

    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k200OK);
    callback(resp);
}

void AiApi::channelUpdateStatus(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 更新渠道状态";
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        Json::Value error;
        error["error"]["message"] = "Invalid JSON in request body";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }

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
        
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
    } catch (const std::exception& e) {
        LOG_ERROR << "[API接口] 更新渠道状态错误: " << e.what();
        Json::Value error;
        error["error"]["message"] = std::string("Error: ") + e.what();
        error["error"]["type"] = "internal_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k500InternalServerError);
        callback(resp);
    }
}

// ===========================================================
// OpenAI Responses API 兼容实现
// ===========================================================
// 响应存储已迁移到 Session 层统一管理

void AiApi::responsesCreate(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 创建响应 - OpenAI Responses API";
    
    // 打印请求头
    LOG_DEBUG << "请求头:";
    for(auto &header : req->getHeaders()) {
        LOG_DEBUG << header.first << ":" << header.second;
    }
    LOG_DEBUG << "请求体:" << req->getBody();

    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        Json::Value error;
        error["error"]["message"] = "Invalid JSON in request body";
        error["error"]["type"] = "invalid_request_error";
        error["error"]["code"] = "invalid_json";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }
    
    auto& reqBody = *jsonPtr;
    const bool stream = reqBody.get("stream", false).asBool();

    // 使用 RequestAdapters 构建 GenerationRequest
    LOG_INFO << "[API接口] 使用 RequestAdapters 构建 GenerationRequest";
    GenerationRequest genReq = RequestAdapters::buildGenerationRequestFromResponses(req);

    // 验证输入不为空
    if (genReq.currentInput.empty() && genReq.messages.empty()) {
        Json::Value error;
        error["error"]["message"] = "Input cannot be empty";
        error["error"]["type"] = "invalid_request_error";
        error["error"]["code"] = "missing_input";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }

    // ========== 非流式：ResponsesJsonSink ==========
    if (!stream) {
        LOG_INFO << "Responses API 非流式请求 - 使用 GenerationService::runGuarded() + ResponsesJsonSink";

        HttpResponsePtr jsonResp;
        int httpStatus = 200;

        ResponsesJsonSink jsonSink(
            [&jsonResp, &httpStatus](const Json::Value& builtResponse, int status) {
                // 成功时存储 response JSON（用于 GET /responses/{id}）
                if (status == 200 && !builtResponse.isMember("error") &&
                    builtResponse.isMember("id") && builtResponse["id"].isString()) {
                    ResponseIndex::instance().storeResponse(builtResponse["id"].asString(), builtResponse);
                }

                jsonResp = HttpResponse::newHttpJsonResponse(builtResponse);
                httpStatus = status;
            },
            genReq.model,
            static_cast<int>(genReq.currentInput.length() / 4)
        );

        GenerationService genService;
        auto gateErr = genService.runGuarded(
            genReq, jsonSink,
            session::ConcurrencyPolicy::RejectConcurrent
        );

        if (gateErr.has_value()) {
            Json::Value errorJson;
            errorJson["error"]["message"] = gateErr->message;
            errorJson["error"]["type"] = gateErr->type();
            errorJson["error"]["code"] = "concurrent_request";
            auto errorResp = HttpResponse::newHttpJsonResponse(errorJson);
            errorResp->setStatusCode(static_cast<HttpStatusCode>(gateErr->httpStatus()));
            callback(errorResp);
            return;
        }

        if (jsonResp) {
            jsonResp->setStatusCode(static_cast<HttpStatusCode>(httpStatus));
            jsonResp->setContentTypeString("application/json; charset=utf-8");
            callback(jsonResp);
        } else {
            Json::Value error;
            error["error"]["message"] = "Failed to generate response";
            error["error"]["type"] = "internal_error";
            auto errorResp = HttpResponse::newHttpJsonResponse(error);
            errorResp->setStatusCode(k500InternalServerError);
            callback(errorResp);
        }

        return;
    }

    // ========== 流式：CollectorSink + ResponsesSseSink ==========
    LOG_INFO << "Responses API 流式请求 - 使用 GenerationService::runGuarded() + ResponsesSseSink";

    CollectorSink collector;
    GenerationService genService;
    auto gateErr = genService.runGuarded(
        genReq, collector,
        session::ConcurrencyPolicy::RejectConcurrent
    );

    if (gateErr.has_value()) {
        Json::Value errorJson;
        errorJson["error"]["message"] = gateErr->message;
        errorJson["error"]["type"] = gateErr->type();
        errorJson["error"]["code"] = "concurrent_request";
        auto errorResp = HttpResponse::newHttpJsonResponse(errorJson);
        errorResp->setStatusCode(static_cast<HttpStatusCode>(gateErr->httpStatus()));
        callback(errorResp);
        return;
    }

    if (collector.hasError()) {
        auto error = collector.getError();
        Json::Value errorResp;
        errorResp["error"]["message"] = error ? error->message : "Internal error";
        errorResp["error"]["type"] = "api_error";
        auto resp = HttpResponse::newHttpJsonResponse(errorResp);
        resp->setStatusCode(HttpStatusCode::k500InternalServerError);
        callback(resp);
        return;
    }

    // 将 Collector 事件复放到 ResponsesJsonSink 构建一份 JSON（用于 GET /responses/{id} 存储）
    Json::Value builtResponse;
    int builtStatus = 200;
    {
        ResponsesJsonSink jsonBuilder(
            [&builtResponse, &builtStatus](const Json::Value& resp, int status) {
                builtResponse = resp;
                builtStatus = status;
            },
            genReq.model,
            static_cast<int>(genReq.currentInput.length() / 4)
        );
        for (const auto& ev : collector.getEvents()) {
            jsonBuilder.onEvent(ev);
        }
        jsonBuilder.onClose();
    }

    if (builtStatus == 200 && !builtResponse.isMember("error") &&
        builtResponse.isMember("id") && builtResponse["id"].isString()) {
        ResponseIndex::instance().storeResponse(builtResponse["id"].asString(), builtResponse);
    }

    // SSE 输出（复放事件给 ResponsesSseSink）
    std::string ssePayload;

    ResponsesSseSink sseSink(
        [&ssePayload](const std::string& chunk) {
            ssePayload += chunk;
            return true;
        },
        []() {},
        genReq.model
    );

    for (const auto& ev : collector.getEvents()) {
        sseSink.onEvent(ev);
    }
    sseSink.onClose();

    struct StreamContext {
        size_t pos = 0;
        time_t start_time = time(nullptr);
        explicit StreamContext() = default;
    };

    auto shared_payload = std::make_shared<std::string>(std::move(ssePayload));
    auto shared_context = std::make_shared<StreamContext>();

    auto resp = HttpResponse::newStreamResponse(
        [shared_payload, shared_context](char *buffer, size_t maxBytes) -> size_t {
            try {
                if (time(nullptr) - shared_context->start_time > 60) {
                    LOG_WARN << "[API接口] Responses SSE 流式传输超时";
                    return 0;
                }

                if (shared_context->pos >= shared_payload->size()) {
                    return 0;
                }

                size_t remaining = shared_payload->size() - shared_context->pos;
                size_t to_send = std::min(remaining, maxBytes);
                memcpy(buffer, shared_payload->data() + shared_context->pos, to_send);
                shared_context->pos += to_send;
                return to_send;
            } catch (const std::exception& e) {
                LOG_ERROR << "[API接口] Responses SSE 流式响应错误: " << e.what();
                return 0;
            }
        }
    );

    resp->setContentTypeString("text/event-stream; charset=utf-8");
    resp->addHeader("Cache-Control", "no-cache");
    resp->addHeader("Connection", "keep-alive");
    resp->addHeader("X-Accel-Buffering", "no");
    resp->addHeader("Keep-Alive", "timeout=60");
    callback(resp);
}

void AiApi::responsesGet(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback, std::string response_id)
{
    LOG_INFO << "[API接口] 获取响应 - 响应ID: " << response_id;

    Json::Value stored;
    if (!ResponseIndex::instance().tryGetResponse(response_id, stored)) {
        Json::Value error;
        error["error"]["message"] = "Response not found";
        error["error"]["type"] = "invalid_request_error";
        error["error"]["code"] = "response_not_found";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k404NotFound);
        callback(resp);
        return;
    }

    auto resp = HttpResponse::newHttpJsonResponse(stored);
    resp->setStatusCode(k200OK);
    callback(resp);
}

void AiApi::responsesDelete(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback, std::string response_id)
{
    LOG_INFO << "[API接口] 删除响应 - 响应ID: " << response_id;

    if (!ResponseIndex::instance().erase(response_id)) {
        Json::Value error;
        error["error"]["message"] = "Response not found";
        error["error"]["type"] = "invalid_request_error";
        error["error"]["code"] = "response_not_found";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k404NotFound);
        callback(resp);
        return;
    }
    
    Json::Value response;
    response["id"] = response_id;
    response["object"] = "response";
    response["deleted"] = true;
    
    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k200OK);
    callback(resp);
    
    LOG_INFO << "[API接口] 删除响应完成, 响应ID: " << response_id;
}

// ========== 错误统计 API ==========

void AiApi::getRequestsSeries(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 获取请求时序统计";
    
    // 解析查询参数
    std::string from = req->getParameter("from");
    std::string to = req->getParameter("to");
    
    // 默认时间范围：最近 24 小时
    if (from.empty() || to.empty()) {
        auto now = std::chrono::system_clock::now();
        auto yesterday = now - std::chrono::hours(24);
        
        auto formatTime = [](std::chrono::system_clock::time_point tp) -> std::string {
            auto tt = std::chrono::system_clock::to_time_t(tp);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::gmtime(&tt));
            return std::string(buf);
        };
        
        if (from.empty()) from = formatTime(yesterday);
        if (to.empty()) to = formatTime(now);
    }
    
    metrics::QueryParams params;
    params.from = from;
    params.to = to;
    
    auto dbManager = metrics::ErrorStatsDbManager::getInstance();
    auto series = dbManager->queryRequestSeries(params);
    
    Json::Value response(Json::objectValue);
    response["from"] = from;
    response["to"] = to;
    
    Json::Value data(Json::arrayValue);
    for (const auto& bucket : series) {
        Json::Value item;
        item["bucket_start"] = bucket.bucketStart;
        item["count"] = static_cast<Json::Int64>(bucket.count);
        data.append(item);
    }
    response["data"] = data;
    
    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k200OK);
    callback(resp);
}

void AiApi::getErrorsSeries(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 获取错误时序统计";
    
    // 解析查询参数
    std::string from = req->getParameter("from");
    std::string to = req->getParameter("to");
    std::string severity = req->getParameter("severity");
    std::string domain = req->getParameter("domain");
    std::string type = req->getParameter("type");
    std::string provider = req->getParameter("provider");
    std::string model = req->getParameter("model");
    std::string clientType = req->getParameter("client_type");
    
    // 默认时间范围：最近 24 小时
    if (from.empty() || to.empty()) {
        auto now = std::chrono::system_clock::now();
        auto yesterday = now - std::chrono::hours(24);
        
        auto formatTime = [](std::chrono::system_clock::time_point tp) -> std::string {
            auto tt = std::chrono::system_clock::to_time_t(tp);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::gmtime(&tt));
            return std::string(buf);
        };
        
        if (from.empty()) from = formatTime(yesterday);
        if (to.empty()) to = formatTime(now);
    }
    
    metrics::QueryParams params;
    params.from = from;
    params.to = to;
    params.severity = severity;
    params.domain = domain;
    params.type = type;
    params.provider = provider;
    params.model = model;
    params.clientType = clientType;
    
    auto dbManager = metrics::ErrorStatsDbManager::getInstance();
    auto series = dbManager->queryErrorSeries(params);
    
    Json::Value response(Json::objectValue);
    response["from"] = from;
    response["to"] = to;
    if (!severity.empty()) response["severity"] = severity;
    if (!domain.empty()) response["domain"] = domain;
    if (!type.empty()) response["type"] = type;
    if (!provider.empty()) response["provider"] = provider;
    if (!model.empty()) response["model"] = model;
    if (!clientType.empty()) response["client_type"] = clientType;
    
    Json::Value data(Json::arrayValue);
    for (const auto& bucket : series) {
        Json::Value item;
        item["bucket_start"] = bucket.bucketStart;
        item["count"] = static_cast<Json::Int64>(bucket.count);
        data.append(item);
    }
    response["data"] = data;
    
    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k200OK);
    callback(resp);
}

void AiApi::getErrorsEvents(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 获取错误事件列表";
    
    // 解析查询参数
    std::string from = req->getParameter("from");
    std::string to = req->getParameter("to");
    int limit = 100;
    int offset = 0;
    
    std::string limitStr = req->getParameter("limit");
    std::string offsetStr = req->getParameter("offset");
    if (!limitStr.empty()) {
        try { limit = std::stoi(limitStr); } catch (...) {}
    }
    if (!offsetStr.empty()) {
        try { offset = std::stoi(offsetStr); } catch (...) {}
    }
    
    // 限制最大返回数量
    if (limit > 1000) limit = 1000;
    if (limit < 1) limit = 1;
    if (offset < 0) offset = 0;
    
    // 默认时间范围：最近 24 小时
    if (from.empty() || to.empty()) {
        auto now = std::chrono::system_clock::now();
        auto yesterday = now - std::chrono::hours(24);
        
        auto formatTime = [](std::chrono::system_clock::time_point tp) -> std::string {
            auto tt = std::chrono::system_clock::to_time_t(tp);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::gmtime(&tt));
            return std::string(buf);
        };
        
        if (from.empty()) from = formatTime(yesterday);
        if (to.empty()) to = formatTime(now);
    }
    
    metrics::QueryParams params;
    params.from = from;
    params.to = to;
    
    auto dbManager = metrics::ErrorStatsDbManager::getInstance();
    auto events = dbManager->queryEvents(params, limit, offset);
    
    Json::Value response(Json::objectValue);
    response["from"] = from;
    response["to"] = to;
    response["limit"] = limit;
    response["offset"] = offset;
    
    Json::Value data(Json::arrayValue);
    for (const auto& ev : events) {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(ev.id);
        item["ts"] = ev.ts;
        item["severity"] = ev.severity;
        item["domain"] = ev.domain;
        item["type"] = ev.type;
        item["provider"] = ev.provider;
        item["model"] = ev.model;
        item["client_type"] = ev.clientType;
        item["api_kind"] = ev.apiKind;
        item["stream"] = ev.stream;
        item["http_status"] = ev.httpStatus;
        item["request_id"] = ev.requestId;
        item["message"] = ev.message;
        data.append(item);
    }
    response["data"] = data;
    response["count"] = static_cast<Json::UInt64>(events.size());
    
    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k200OK);
    callback(resp);
}

void AiApi::getErrorsEventById(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback, int64_t id)
{
    LOG_INFO << "[API接口] 获取错误事件详情 - ID: " << id;
    
    auto dbManager = metrics::ErrorStatsDbManager::getInstance();
    auto eventOpt = dbManager->queryEventById(id);
    
    if (!eventOpt.has_value()) {
        Json::Value error;
        error["error"]["message"] = "Event not found";
        error["error"]["type"] = "invalid_request_error";
        error["error"]["code"] = "event_not_found";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k404NotFound);
        callback(resp);
        return;
    }
    
    const auto& ev = eventOpt.value();
    
    Json::Value response;
    response["id"] = static_cast<Json::Int64>(ev.id);
    response["ts"] = ev.ts;
    response["severity"] = ev.severity;
    response["domain"] = ev.domain;
    response["type"] = ev.type;
    response["provider"] = ev.provider;
    response["model"] = ev.model;
    response["client_type"] = ev.clientType;
    response["api_kind"] = ev.apiKind;
    response["stream"] = ev.stream;
    response["http_status"] = ev.httpStatus;
    response["request_id"] = ev.requestId;
    response["message"] = ev.message;
    response["detail_json"] = ev.detailJson;
    response["raw_snippet"] = ev.rawSnippet;
    
    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k200OK);
    callback(resp);
}

// ========== 服务状态监控 API ==========

void AiApi::getStatusSummary(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 获取服务状态概览";
    
    // 解析查询参数
    std::string from = req->getParameter("from");
    std::string to = req->getParameter("to");
    
    metrics::StatusQueryParams params;
    params.from = from;
    params.to = to;
    
    auto statusManager = metrics::StatusDbManager::getInstance();
    auto summary = statusManager->getStatusSummary(params);
    
    Json::Value response(Json::objectValue);
    response["total_requests"] = static_cast<Json::Int64>(summary.totalRequests);
    response["total_errors"] = static_cast<Json::Int64>(summary.totalErrors);
    response["error_rate"] = summary.errorRate;
    response["channel_count"] = summary.channelCount;
    response["model_count"] = summary.modelCount;
    response["healthy_channels"] = summary.healthyChannels;
    response["degraded_channels"] = summary.degradedChannels;
    response["down_channels"] = summary.downChannels;
    response["overall_status"] = metrics::StatusDbManager::statusToString(summary.overallStatus);
    
    // 时间序列数据
    Json::Value buckets(Json::arrayValue);
    for (const auto& bucket : summary.buckets) {
        Json::Value item;
        item["bucket_start"] = bucket.bucketStart;
        item["request_count"] = static_cast<Json::Int64>(bucket.requestCount);
        item["error_count"] = static_cast<Json::Int64>(bucket.errorCount);
        item["error_rate"] = bucket.errorRate;
        buckets.append(item);
    }
    response["buckets"] = buckets;
    
    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k200OK);
    callback(resp);
}

void AiApi::getStatusChannels(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 获取渠道状态列表";
    
    // 解析查询参数
    std::string from = req->getParameter("from");
    std::string to = req->getParameter("to");
    std::string provider = req->getParameter("provider");
    
    metrics::StatusQueryParams params;
    params.from = from;
    params.to = to;
    params.provider = provider;
    
    auto statusManager = metrics::StatusDbManager::getInstance();
    auto channels = statusManager->getChannelStatusList(params);
    
    Json::Value response(Json::objectValue);
    Json::Value data(Json::arrayValue);
    
    for (const auto& ch : channels) {
        Json::Value item;
        item["channel_id"] = ch.channelId;
        item["channel_name"] = ch.channelName;
        item["total_requests"] = static_cast<Json::Int64>(ch.totalRequests);
        item["total_errors"] = static_cast<Json::Int64>(ch.totalErrors);
        item["error_rate"] = ch.errorRate;
        item["status"] = metrics::StatusDbManager::statusToString(ch.status);
        item["last_request_time"] = ch.lastRequestTime;
        
        // 时间序列数据
        Json::Value buckets(Json::arrayValue);
        for (const auto& bucket : ch.buckets) {
            Json::Value b;
            b["bucket_start"] = bucket.bucketStart;
            b["request_count"] = static_cast<Json::Int64>(bucket.requestCount);
            b["error_count"] = static_cast<Json::Int64>(bucket.errorCount);
            b["error_rate"] = bucket.errorRate;
            buckets.append(b);
        }
        item["buckets"] = buckets;
        
        data.append(item);
    }
    
    response["data"] = data;
    response["count"] = static_cast<Json::UInt64>(channels.size());
    
    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k200OK);
    callback(resp);
}

void AiApi::getStatusModels(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 获取模型状态列表";
    
    // 解析查询参数
    std::string from = req->getParameter("from");
    std::string to = req->getParameter("to");
    std::string provider = req->getParameter("provider");
    std::string model = req->getParameter("model");
    
    metrics::StatusQueryParams params;
    params.from = from;
    params.to = to;
    params.provider = provider;
    params.model = model;
    
    auto statusManager = metrics::StatusDbManager::getInstance();
    auto models = statusManager->getModelStatusList(params);
    
    Json::Value response(Json::objectValue);
    Json::Value data(Json::arrayValue);
    
    for (const auto& m : models) {
        Json::Value item;
        item["model"] = m.model;
        item["provider"] = m.provider;
        item["total_requests"] = static_cast<Json::Int64>(m.totalRequests);
        item["total_errors"] = static_cast<Json::Int64>(m.totalErrors);
        item["error_rate"] = m.errorRate;
        item["status"] = metrics::StatusDbManager::statusToString(m.status);
        item["last_request_time"] = m.lastRequestTime;
        
        // 时间序列数据
        Json::Value buckets(Json::arrayValue);
        for (const auto& bucket : m.buckets) {
            Json::Value b;
            b["bucket_start"] = bucket.bucketStart;
            b["request_count"] = static_cast<Json::Int64>(bucket.requestCount);
            b["error_count"] = static_cast<Json::Int64>(bucket.errorCount);
            b["error_rate"] = bucket.errorRate;
            buckets.append(b);
        }
        item["buckets"] = buckets;
        
        data.append(item);
    }
    
    response["data"] = data;
    response["count"] = static_cast<Json::UInt64>(models.size());
    
    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k200OK);
    callback(resp);
}
// ========== 日志查看 API ==========

void AiApi::logsList(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 获取日志文件列表";

    Json::Value response(Json::arrayValue);
    std::string logDir = "logs";

    try {
        for (const auto& entry : std::filesystem::directory_iterator(logDir)) {
            if (entry.is_regular_file()) {
                Json::Value fileInfo;
                fileInfo["name"] = entry.path().filename().string();
                fileInfo["size"] = static_cast<Json::Int64>(entry.file_size());

                auto ftime = entry.last_write_time();
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
                );
                auto tt = std::chrono::system_clock::to_time_t(sctp);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&tt));
                fileInfo["modified"] = std::string(buf);

                response.append(fileInfo);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "[API接口] 列出日志文件失败: " << e.what();
    }

    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k200OK);
    callback(resp);
}

void AiApi::logsTail(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[API接口] 读取日志尾部内容";

    std::string fileName = req->getParameter("file");
    if (fileName.empty()) fileName = "aiapi.log";

    // 安全检查：防止路径遍历
    if (fileName.find("..") != std::string::npos || fileName.find('/') != std::string::npos || fileName.find('\\') != std::string::npos) {
        Json::Value error;
        error["error"]["message"] = "Invalid file name";
        error["error"]["type"] = "invalid_request_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k400BadRequest);
        callback(resp);
        return;
    }

    int lines = 200;
    std::string linesStr = req->getParameter("lines");
    if (!linesStr.empty()) {
        try { lines = std::stoi(linesStr); } catch (...) {}
    }
    if (lines < 1) lines = 1;
    if (lines > 5000) lines = 5000;

    std::string keyword = req->getParameter("keyword");
    std::string level = req->getParameter("level");

    std::string filePath = "logs/" + fileName;

    std::ifstream file(filePath, std::ios::ate);
    if (!file.is_open()) {
        Json::Value error;
        error["error"]["message"] = "Log file not found: " + fileName;
        error["error"]["type"] = "not_found";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k404NotFound);
        callback(resp);
        return;
    }

    // 从文件尾部读取
    std::streampos fileSize = file.tellg();
    std::vector<std::string> allLines;

    // 读取文件尾部足够多的内容
    size_t bufSize = std::min(static_cast<size_t>(fileSize), static_cast<size_t>(lines * 500));
    file.seekg(-static_cast<std::streamoff>(bufSize), std::ios::end);

    std::string line;
    // 跳过可能不完整的第一行
    if (file.tellg() != std::streampos(0)) {
        std::getline(file, line);
    }

    while (std::getline(file, line)) {
        // 日志级别过滤
        if (!level.empty() && level != "ALL") {
            if (line.find(level) == std::string::npos) continue;
        }
        // 关键词过滤
        if (!keyword.empty()) {
            if (line.find(keyword) == std::string::npos) continue;
        }
        allLines.push_back(line);
    }
    file.close();

    // 只保留最后 N 行
    int startIdx = 0;
    if (static_cast<int>(allLines.size()) > lines) {
        startIdx = static_cast<int>(allLines.size()) - lines;
    }

    Json::Value response;
    response["file"] = fileName;
    response["total_lines"] = static_cast<Json::Int64>(allLines.size());
    response["returned_lines"] = static_cast<Json::Int64>(allLines.size() - startIdx);

    Json::Value linesArray(Json::arrayValue);
    for (int i = startIdx; i < static_cast<int>(allLines.size()); ++i) {
        linesArray.append(allLines[i]);
    }
    response["lines"] = linesArray;

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&tt));
    response["timestamp"] = std::string(buf);

    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k200OK);
    callback(resp);
}
