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
#include <channelManager/channelManager.h>
#include <sessionManager/Session.h>
#include <sessionManager/ClientOutputSanitizer.h>
#include <sessionManager/GenerationService.h>
#include <sessionManager/GenerationRequest.h>
#include <sessionManager/IResponseSink.h>
#include <sessionManager/SessionExecutionGate.h>
#include <sessionManager/Errors.h>
#include <sessionManager/RequestAdapters.h>
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

using namespace drogon;
using namespace drogon::orm;

// Add definition of your processing function here
// 响应存储已迁移到 Session 层统一管理
void AiApi::chaynsapichat(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    //打印所有的请求头
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
    LOG_INFO << "[API接口] 会话Key: " << genReq.sessionKey;
    
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
    for(auto &item:reqItems)
    {
        Accountinfo_st accountinfo;
        Json::Value responseitem;
        accountinfo.apiName=item["apiname"].asString();
        accountinfo.userName=item["username"].asString();
        responseitem["apiname"]=accountinfo.apiName;
        responseitem["username"]=accountinfo.userName;

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
    bool stream = reqBody.get("stream", false).asBool();
    
    // ========== PR3: 使用 RequestAdapters 构建 GenerationRequest ==========
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
    
    // ========== Responses API：与 Chat 完全对齐 ==========
    // - stream=false: ResponsesJsonSink（一次性 JSON）
    // - stream=true : ResponsesSseSink（SSE）

    // 预先生成 responseId，确保 Controller / GenerationService / Session 存储一致
    std::string responseId = chatSession::generateResponseId();
    genReq.responseId = responseId;

    // ========== 非流式：ResponsesJsonSink ==========
    if (!stream) {
        LOG_INFO << "Responses API 非流式请求 - 使用 GenerationService::runGuarded() + ResponsesJsonSink";

        HttpResponsePtr jsonResp;
        int httpStatus = 200;

        ResponsesJsonSink jsonSink(
            [&jsonResp, &httpStatus, &responseId](const Json::Value& builtResponse, int status) {
                // 存储完整 response（保留 _internal_session_id）到已存在的 response_id 会话中
                if (status == 200 && !builtResponse.isMember("error")) {
                    chatSession::getInstance()->updateResponseApiData(responseId, builtResponse);
                }

                Json::Value publicResponse = builtResponse;
                publicResponse.removeMember("_internal_session_id");

                jsonResp = HttpResponse::newHttpJsonResponse(publicResponse);
                httpStatus = status;
            },
            responseId,
            genReq.model,
            genReq.sessionKey,
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

        LOG_INFO << "[API接口] 创建响应完成, 响应ID: " << responseId;
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

    // 组装一份 JSON 响应用于 Session 存储（对外不直接返回它；对外返回 SSE）
    std::string responseMessage = collector.getFinalText();
    std::string modelUsed = genReq.model;
    std::string sessionId = genReq.sessionKey;

    Json::Value response;
    response["id"] = responseId;
    response["object"] = "response";
    response["created_at"] = static_cast<Json::Int64>(time(nullptr));
    response["model"] = modelUsed;
    response["status"] = "completed";

    Json::Value outputArray(Json::arrayValue);
    Json::Value messageOutput;
    messageOutput["type"] = "message";
    messageOutput["id"] = "msg_" + responseId;
    messageOutput["status"] = "completed";
    messageOutput["role"] = "assistant";

    Json::Value contentArray(Json::arrayValue);
    if (!responseMessage.empty()) {
        Json::Value textContent;
        textContent["type"] = "output_text";
        textContent["text"] = responseMessage;
        contentArray.append(textContent);
    }
    messageOutput["content"] = contentArray;

    outputArray.append(messageOutput);
    response["output"] = outputArray;

    // usage：优先从 Completed 事件获取，否则估算
    std::optional<generation::Usage> usageOpt;
    for (const auto& ev : collector.getEvents()) {
        if (std::holds_alternative<generation::Completed>(ev)) {
            const auto& c = std::get<generation::Completed>(ev);
            if (c.usage.has_value()) {
                usageOpt = *c.usage;
            }
        }
    }

    Json::Value usage;
    if (usageOpt.has_value()) {
        usage["input_tokens"] = usageOpt->inputTokens;
        usage["output_tokens"] = usageOpt->outputTokens;
        usage["total_tokens"] = usageOpt->totalTokens;
    } else {
        usage["input_tokens"] = static_cast<int>(genReq.currentInput.length() / 4);
        usage["output_tokens"] = static_cast<int>(responseMessage.length() / 4);
        usage["total_tokens"] = usage["input_tokens"].asInt() + usage["output_tokens"].asInt();
    }
    response["usage"] = usage;

    response["_internal_session_id"] = sessionId;

    // 存储完整 response（保留 _internal_session_id）到已存在的 response_id 会话中
    chatSession::getInstance()->updateResponseApiData(responseId, response);

    // SSE 输出
    std::string ssePayload;

    ResponsesSseSink sseSink(
        [&ssePayload](const std::string& chunk) {
            ssePayload += chunk;
            return true;
        },
        []() {},
        responseId,
        modelUsed
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
    
    
    LOG_INFO << "[API接口] 创建响应完成, 响应ID: " << responseId;
}

void AiApi::responsesGet(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback, std::string response_id)
{
    LOG_INFO << "[API接口] 获取响应 - 响应ID: " << response_id;
    
    session_st responseSession;
    if (!chatSession::getInstance()->getResponseSession(response_id, responseSession)) {
        Json::Value error;
        error["error"]["message"] = "Response not found";
        error["error"]["type"] = "invalid_request_error";
        error["error"]["code"] = "response_not_found";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k404NotFound);
        callback(resp);
        return;
    }
    
    // 移除内部字段后返回
    Json::Value publicResponse = responseSession.api_response_data;
    publicResponse.removeMember("_internal_session_id");
    
    auto resp = HttpResponse::newHttpJsonResponse(publicResponse);
    resp->setStatusCode(k200OK);
    callback(resp);
}

void AiApi::responsesDelete(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback, std::string response_id)
{
    LOG_INFO << "[API接口] 删除响应 - 响应ID: " << response_id;
    
    if (!chatSession::getInstance()->deleteResponseSession(response_id)) {
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
