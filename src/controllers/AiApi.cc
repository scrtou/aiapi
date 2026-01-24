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
#include <drogon/orm/Exception.h>
#include <drogon/orm/DbClient.h>
#include <vector> // 添加 vector 头文件
#include <ctime>
#include <iomanip>
#include <sstream>

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
    
    // 获取响应内容
    std::string message;
    int statusCode = 200;
    
    if (collector.hasError()) {
        auto error = collector.getError();
        statusCode = 400;
        message = error->message;
        LOG_ERROR << "GenerationService 返回错误: " << error->message;
    } else {
        message = collector.getFinalText();
        // 注意: 清洗已在 GenerationService::emitResultEvents() 中完成，此处不再重复调用
    }
    LOG_INFO << "回复message长度:" << message.length();

    HttpResponsePtr resp;
    std::string oldConversationId = genReq.sessionKey;
    
    if (statusCode == 200)
    {
            {
            LOG_INFO << "流式响应";
            // 创建一个新的响应对象
            // 创建一个持久的上下文对象

            struct StreamContext {
                    size_t pos = 0;
                    bool sent_final_chunk = false;
                    bool sent_done = false;
                    std::string response_message;
                    time_t start_time;
                    bool first_chunk = true;
                    StreamContext(const std::string& msg) :
                    response_message(msg),
                    start_time(time(nullptr)) {}
                };

            // 在lambda外创建上下文，并使用shared_ptr来共享
            // 注意：这里使用的是经过处理（可能被清洗或包裹）后的 message
            auto shared_context = std::make_shared<StreamContext>(message);
            
            resp = HttpResponse::newStreamResponse([shared_context, oldConversationId](char *buffer, size_t maxBytes) -> size_t {
                try {
                    if (shared_context->sent_done) {
                        return 0;
                    }

                    if (time(nullptr) - shared_context->start_time > 60) {
                        LOG_WARN << "[API接口] 流式传输超时";
                        return 0;
                    }

                    // After all content is sent, send the final chunk, then [DONE]
                    if (shared_context->pos >= shared_context->response_message.length()) {
                        if (!shared_context->sent_final_chunk) {
                            Json::Value data;
                            data["id"] = "chatcmpl-" + oldConversationId.substr(0, 5) + "-" + std::to_string(time(nullptr));
                            data["object"] = "chat.completion.chunk";
                            data["created"] = static_cast<int>(time(nullptr));
                            data["choices"][0]["index"] = 0;
                            data["choices"][0]["delta"] = Json::objectValue;
                            data["choices"][0]["finish_reason"] = "stop";

                            Json::StreamWriterBuilder writer_builder;
                            writer_builder["indentation"] = "";
                            writer_builder["emitUTF8"] = true;
                            std::string json_str = Json::writeString(writer_builder, data);
                            
                            std::string final_chunk_str = "data: " + json_str + "\n\n";
                            size_t to_send = std::min(final_chunk_str.length(), maxBytes);
                            memcpy(buffer, final_chunk_str.c_str(), to_send);
                            shared_context->sent_final_chunk = true;
                            return to_send;
                        } else {
                            const std::string done_message = "data: [DONE]\n\n";
                            size_t done_size = std::min(done_message.length(), maxBytes);
                            memcpy(buffer, done_message.c_str(), done_size);
                            shared_context->sent_done = true;
                            return done_size;
                        }
                    }

                    // Calculate chunk size (UTF-8 safe)
                    size_t chunk_size = 0;
                    size_t remaining = shared_context->response_message.length() - shared_context->pos;
                    size_t target_size = std::min(remaining, size_t(30)); // Increased chunk size
                    
                    while (chunk_size < target_size) {
                        if (shared_context->pos + chunk_size >= shared_context->response_message.length()) break;
                        unsigned char c = shared_context->response_message[shared_context->pos + chunk_size];
                        int char_len = 0;
                        if ((c & 0x80) == 0) char_len = 1;
                        else if ((c & 0xE0) == 0xC0) char_len = 2;
                        else if ((c & 0xF0) == 0xE0) char_len = 3;
                        else if ((c & 0xF8) == 0xF0) char_len = 4;
                        else { chunk_size++; continue; }

                        if (chunk_size + char_len > target_size) break;
                        chunk_size += char_len;
                    }
                    if (chunk_size == 0 && remaining > 0) chunk_size = remaining;


                    // Construct SSE message
                    Json::Value data;
                    data["id"] = "chatcmpl-" + oldConversationId.substr(0, 5) + "-" + std::to_string(time(nullptr));
                    data["object"] = "chat.completion.chunk";
                    data["created"] = static_cast<int>(time(nullptr));
                    data["choices"][0]["index"] = 0;

                    if (shared_context->first_chunk) {
                        data["choices"][0]["delta"]["role"] = "assistant";
                        shared_context->first_chunk = false;
                    }
                    
                    data["choices"][0]["delta"]["content"] = shared_context->response_message.substr(shared_context->pos, chunk_size);
                    data["choices"][0]["finish_reason"] = Json::Value();

                    Json::StreamWriterBuilder writer_builder;
                    writer_builder["indentation"] = "";
                    writer_builder["emitUTF8"] = true;
                    std::string json_str = Json::writeString(writer_builder, data);
                    
                    std::string chunk_str = "data: " + json_str + "\n\n";
                    size_t to_send = std::min(chunk_str.length(), maxBytes);
                    memcpy(buffer, chunk_str.c_str(), to_send);
                    
                    shared_context->pos += chunk_size;
                    LOG_DEBUG << "[API接口] 发送进度: " << shared_context->pos << "/" << shared_context->response_message.length();
                    
                    //std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    
                    return to_send;
                    
                } catch (const std::exception& e) {
                    LOG_ERROR << "[API接口] 流式响应错误: " << e.what();
                    return 0;
                }
            });
            
            // 设置响应头
            resp->setContentTypeString("text/event-stream; charset=utf-8");
            resp->addHeader("Cache-Control", "no-cache");
            resp->addHeader("Connection", "keep-alive");
            resp->addHeader("X-Accel-Buffering", "no");
            resp->addHeader("Keep-Alive", "timeout=60");
            callback(resp);
            
            // 注意：会话更新和afterResponseProcess已在 GenerationService::runWithSessionGuarded() 中完成
            // 不需要在这里重复调用
            LOG_INFO << "流式响应发送完成";
        }
    }
    else
    {   
        // ... error handling ...
        LOG_INFO << "非流式响应,错误码:"<<statusCode;
            Json::Value response;
        Json::Value error;
        error["error"]["message"] = "Failed to get response from chaynsapi";
        error["error"]["type"] = "invalid_request_error";
        response["error"]=error;
        resp = HttpResponse::newHttpJsonResponse(response);
        resp->setStatusCode(k400BadRequest);
        resp->setContentTypeString("application/json; charset=utf-8");
        callback(resp);
    }
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
    
    std::string responseMessage;
    std::string modelUsed;
    std::string sessionId;
    int statusCode = 200;
    
    // ========== 使用 GenerationService::runGuarded() 统一入口 ==========
    LOG_INFO << "Responses API " << (stream ? "流式" : "非流式") << "请求 - 使用 GenerationService::runGuarded()";
    
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
        errorJson["error"]["code"] = "concurrent_request";
        auto errorResp = HttpResponse::newHttpJsonResponse(errorJson);
        errorResp->setStatusCode(static_cast<HttpStatusCode>(gateErr->httpStatus()));
        callback(errorResp);
        return;
    }
    
    // 检查是否有错误
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
    
    // 从 CollectorSink 获取最终文本
    responseMessage = collector.getFinalText();
    modelUsed = genReq.model;
    sessionId = genReq.sessionKey;
    
    // 构建 Responses API 格式的响应
    std::string responseId = chatSession::generateResponseId();
    int64_t createdAt = static_cast<int64_t>(time(nullptr));
    
    Json::Value response;
    response["id"] = responseId;
    response["object"] = "response";
    response["created_at"] = createdAt;
    response["model"] = modelUsed;
    response["status"] = "completed";
    
    // 构建 output 数组
    Json::Value outputArray(Json::arrayValue);
    
    // 添加 message 输出项
    Json::Value messageOutput;
    messageOutput["type"] = "message";
    messageOutput["id"] = "msg_" + responseId.substr(5);
    messageOutput["status"] = "completed";
    messageOutput["role"] = "assistant";
    
    // 添加 content 数组
    Json::Value contentArray(Json::arrayValue);
    Json::Value textContent;
    textContent["type"] = "output_text";
    textContent["text"] = responseMessage;
    contentArray.append(textContent);
    messageOutput["content"] = contentArray;
    
    outputArray.append(messageOutput);
    response["output"] = outputArray;
    
    // 添加 usage 信息 (估算)
    Json::Value usage;
    usage["input_tokens"] = static_cast<int>(genReq.currentInput.length() / 4);
    usage["output_tokens"] = static_cast<int>(responseMessage.length() / 4);
    usage["total_tokens"] = usage["input_tokens"].asInt() + usage["output_tokens"].asInt();
    response["usage"] = usage;
    
    // 添加内部会话ID用于后续对话
    response["_internal_session_id"] = sessionId;
    
    // 存储响应到 Session 层
    session_st responseSession;
    responseSession.response_id = responseId;
    responseSession.api_response_data = response;
    responseSession.is_response_api = true;
    responseSession.created_time = time(nullptr);
    responseSession.last_active_time = time(nullptr);
    chatSession::getInstance()->createResponseSession(responseSession);
    
    // 移除内部字段后返回
    Json::Value publicResponse = response;
    publicResponse.removeMember("_internal_session_id");
    
    // ======= stream 分支：替换你原来的整个 if (stream) { ... } 代码块即可 =======
    if (stream) {
        auto shared_response   = std::make_shared<std::string>(responseMessage);
        auto shared_pos        = std::make_shared<size_t>(0);
    
        // phases:
        // 0 created
        // 1 in_progress
        // 2 output_item.added
        // 3 content_part.added
        // 4 delta loop
        // 5 output_text.done
        // 6 content_part.done
        // 7 output_item.done
        // 8 response.completed
        // 9 done
        auto shared_phase      = std::make_shared<int>(0);
    
        auto shared_responseId = std::make_shared<std::string>(responseId);
        auto shared_model      = std::make_shared<std::string>(modelUsed);
        auto shared_msgId      = std::make_shared<std::string>("msg_" + responseId.substr(5));
        auto shared_seqNum     = std::make_shared<int>(0);
    
        auto shared_usage = std::make_shared<Json::Value>();
        (*shared_usage)["input_tokens"]  = static_cast<int>(genReq.currentInput.length() / 4);
        (*shared_usage)["output_tokens"] = static_cast<int>(responseMessage.length() / 4);
        (*shared_usage)["total_tokens"]  = (*shared_usage)["input_tokens"].asInt() + (*shared_usage)["output_tokens"].asInt();
    
        auto shared_sendBuffer = std::make_shared<std::string>();
        auto shared_sendPos    = std::make_shared<size_t>(0);
    
        auto resp = HttpResponse::newStreamResponse(
            [shared_response, shared_pos, shared_phase,
             shared_responseId, shared_model, shared_msgId,
             shared_seqNum, shared_sendBuffer, shared_sendPos,
             shared_usage, createdAt](char* buffer, size_t maxBytes) -> size_t {
    
                // 1) 先把上次没发完的 buffer 发完
                if (*shared_sendPos < shared_sendBuffer->length()) {
                    size_t remaining = shared_sendBuffer->length() - *shared_sendPos;
                    size_t toSend = std::min(remaining, maxBytes);
                    memcpy(buffer, shared_sendBuffer->c_str() + *shared_sendPos, toSend);
                    *shared_sendPos += toSend;
                    return toSend;
                }
    
                Json::StreamWriterBuilder writer;
                writer["indentation"] = "";
                writer["emitUTF8"] = true;
    
                auto sse = [&](const std::string& eventName, const Json::Value& json) -> std::string {
                    std::string data = Json::writeString(writer, json);
                    return "event: " + eventName + "\n" + "data: " + data + "\n\n";
                };
    
                std::string eventStr;
    
                // 2) 生成“下一条事件”（只生成一条，交给框架多次回调逐条发）
                if (*shared_phase == 0) {
                    Json::Value event;
                    event["type"] = "response.created";
                    event["sequence_number"] = (*shared_seqNum)++;
    
                    Json::Value respObj;
                    respObj["id"] = *shared_responseId;
                    respObj["object"] = "response";
                    respObj["status"] = "in_progress";
                    respObj["model"] = *shared_model;
                    respObj["created_at"] = static_cast<int64_t>(createdAt);
                    respObj["completed_at"] = Json::nullValue;
                    respObj["error"] = Json::nullValue;
                    respObj["metadata"] = Json::Value(Json::objectValue);
                    respObj["output"] = Json::Value(Json::arrayValue);
                    respObj["usage"] = Json::nullValue;
    
                    event["response"] = respObj;
                    eventStr = sse("response.created", event);
                    *shared_phase = 1;
    
                } else if (*shared_phase == 1) {
                    Json::Value event;
                    event["type"] = "response.in_progress";
                    event["sequence_number"] = (*shared_seqNum)++;
    
                    Json::Value respObj;
                    respObj["id"] = *shared_responseId;
                    respObj["object"] = "response";
                    respObj["status"] = "in_progress";
                    respObj["model"] = *shared_model;
                    respObj["created_at"] = static_cast<int64_t>(createdAt);
                    respObj["completed_at"] = Json::nullValue;
                    respObj["error"] = Json::nullValue;
                    respObj["metadata"] = Json::Value(Json::objectValue);
                    respObj["output"] = Json::Value(Json::arrayValue);
                    respObj["usage"] = Json::nullValue;
    
                    event["response"] = respObj;
                    eventStr = sse("response.in_progress", event);
                    *shared_phase = 2;
    
                } else if (*shared_phase == 2) {
                    Json::Value event;
                    event["type"] = "response.output_item.added";
                    event["sequence_number"] = (*shared_seqNum)++;
                    event["output_index"] = 0;
    
                    Json::Value item;
                    item["id"] = *shared_msgId;
                    item["type"] = "message";
                    item["status"] = "in_progress";
                    item["role"] = "assistant";
                    item["content"] = Json::Value(Json::arrayValue);
    
                    event["item"] = item;
                    eventStr = sse("response.output_item.added", event);
                    *shared_phase = 3;
    
                } else if (*shared_phase == 3) {
                    Json::Value event;
                    event["type"] = "response.content_part.added";
                    event["sequence_number"] = (*shared_seqNum)++;
                    event["item_id"] = *shared_msgId;
                    event["output_index"] = 0;
                    event["content_index"] = 0;
    
                    Json::Value part;
                    part["type"] = "output_text";
                    part["text"] = "";
                    part["annotations"] = Json::Value(Json::arrayValue);
    
                    event["part"] = part;
                    eventStr = sse("response.content_part.added", event);
                    *shared_phase = 4;
    
                } else if (*shared_phase == 4) {
                    // ====== 关键修复点：这里绝不能 return 0 来“等下一次” ======
                    // 如果内容已经发完，则立刻进入 phase=5，让下一次回调发 done
                    if (*shared_pos >= shared_response->length()) {
                        *shared_phase = 5;
                        // 这一次我们不结束流，而是发一个很小的“空 delta”也可以不发。
                        // 为了严格按规范，这里直接构造 output_text.done（等下一次也行）。
                        // 这里选择：本次就发 output_text.done，避免客户端因连接太快结束而漏事件。
                        Json::Value event;
                        event["type"] = "response.output_text.done";
                        event["sequence_number"] = (*shared_seqNum)++;
                        event["item_id"] = *shared_msgId;
                        event["output_index"] = 0;
                        event["content_index"] = 0;
                        event["text"] = *shared_response;
    
                        eventStr = sse("response.output_text.done", event);
                        *shared_phase = 6;
                    } else {
                        // 发 delta
                        size_t remaining = shared_response->length() - *shared_pos;
                        size_t targetSize = std::min(remaining, size_t(40));
                        size_t chunkSize = 0;
    
                        while (chunkSize < targetSize) {
                            if (*shared_pos + chunkSize >= shared_response->length()) break;
                            unsigned char c = static_cast<unsigned char>((*shared_response)[*shared_pos + chunkSize]);
                            int charLen = 1;
                            if ((c & 0x80) == 0) charLen = 1;
                            else if ((c & 0xE0) == 0xC0) charLen = 2;
                            else if ((c & 0xF0) == 0xE0) charLen = 3;
                            else if ((c & 0xF8) == 0xF0) charLen = 4;
    
                            if (chunkSize + static_cast<size_t>(charLen) > targetSize) break;
                            chunkSize += static_cast<size_t>(charLen);
                        }
                        if (chunkSize == 0 && remaining > 0) chunkSize = remaining;
    
                        std::string chunk = shared_response->substr(*shared_pos, chunkSize);
                        *shared_pos += chunkSize;
    
                        Json::Value event;
                        event["type"] = "response.output_text.delta";
                        event["sequence_number"] = (*shared_seqNum)++;
                        event["item_id"] = *shared_msgId;
                        event["output_index"] = 0;
                        event["content_index"] = 0;
                        event["delta"] = chunk;
    
                        eventStr = sse("response.output_text.delta", event);
    
                        // 如果这一段发完刚好结束，下次回调会进入上面的 done 分支
                    }
    
                } else if (*shared_phase == 6) {
                    Json::Value event;
                    event["type"] = "response.content_part.done";
                    event["sequence_number"] = (*shared_seqNum)++;
                    event["item_id"] = *shared_msgId;
                    event["output_index"] = 0;
                    event["content_index"] = 0;
    
                    Json::Value part;
                    part["type"] = "output_text";
                    part["text"] = *shared_response;
                    part["annotations"] = Json::Value(Json::arrayValue);
    
                    event["part"] = part;
                    eventStr = sse("response.content_part.done", event);
                    *shared_phase = 7;
    
                } else if (*shared_phase == 7) {
                    Json::Value event;
                    event["type"] = "response.output_item.done";
                    event["sequence_number"] = (*shared_seqNum)++;
                    event["output_index"] = 0;
    
                    Json::Value item;
                    item["id"] = *shared_msgId;
                    item["type"] = "message";
                    item["status"] = "completed";
                    item["role"] = "assistant";
    
                    Json::Value contentArr(Json::arrayValue);
                    Json::Value content;
                    content["type"] = "output_text";
                    content["text"] = *shared_response;
                    content["annotations"] = Json::Value(Json::arrayValue);
                    contentArr.append(content);
    
                    item["content"] = contentArr;
                    event["item"] = item;
    
                    eventStr = sse("response.output_item.done", event);
                    *shared_phase = 8;
    
                } else if (*shared_phase == 8) {
                    Json::Value event;
                    event["type"] = "response.completed";
                    event["sequence_number"] = (*shared_seqNum)++;
    
                    Json::Value respObj;
                    respObj["id"] = *shared_responseId;
                    respObj["object"] = "response";
                    respObj["status"] = "completed";
                    respObj["model"] = *shared_model;
                    respObj["created_at"] = static_cast<int64_t>(createdAt);
                    respObj["completed_at"] = static_cast<int64_t>(time(nullptr));
                    respObj["error"] = Json::nullValue;
                    respObj["metadata"] = Json::Value(Json::objectValue);
    
                    Json::Value outputArr(Json::arrayValue);
                    Json::Value outputItem;
                    outputItem["id"] = *shared_msgId;
                    outputItem["type"] = "message";
                    outputItem["status"] = "completed";
                    outputItem["role"] = "assistant";
    
                    Json::Value contentArr(Json::arrayValue);
                    Json::Value content;
                    content["type"] = "output_text";
                    content["text"] = *shared_response;
                    content["annotations"] = Json::Value(Json::arrayValue);
                    contentArr.append(content);
    
                    outputItem["content"] = contentArr;
                    outputArr.append(outputItem);
    
                    respObj["output"] = outputArr;
                    respObj["usage"] = *shared_usage;
    
                    event["response"] = respObj;
                    eventStr = sse("response.completed", event);
                    *shared_phase = 9;
    
                } else {
                    // 只有这里才 return 0 结束流
                    return 0;
                }
    
                // 3) 把事件塞进发送缓冲区
                *shared_sendBuffer = eventStr;
                *shared_sendPos = 0;
    
                // 4) 发出去
                size_t toSend = std::min(shared_sendBuffer->length(), maxBytes);
                memcpy(buffer, shared_sendBuffer->c_str(), toSend);
                *shared_sendPos = toSend;
                return toSend;
            }
        );
    
        resp->setContentTypeString("text/event-stream; charset=utf-8");
        resp->addHeader("Cache-Control", "no-cache");
        resp->addHeader("Connection", "keep-alive");
        resp->addHeader("X-Accel-Buffering", "no");
        callback(resp);
    } else {
        auto resp = HttpResponse::newHttpJsonResponse(publicResponse);
        resp->setStatusCode(k200OK);
        resp->setContentTypeString("application/json; charset=utf-8");
        callback(resp);
    }
    
    
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
