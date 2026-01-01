#include "AiApi.h"
#include <drogon/HttpResponse.h>
#include <json/json.h>
#include<drogon/drogon.h>
#include <unistd.h>
#include <apiManager/ApiManager.h>
#include <accountManager/accountManager.h>
#include <dbManager/account/accountDbManager.h>
#include <sessionManager/Session.h>
#include <drogon/orm/Exception.h>
#include <drogon/orm/DbClient.h>
using namespace drogon;
using namespace drogon::orm;

// Add definition of your processing function here
void AiApi::chaynsapichat(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    //打印所有的请求头
    //打印请求头
    LOG_DEBUG<<"请求头:";
    for(auto &header : req->getHeaders())
    {
        LOG_DEBUG<<header.first<<":"<<header.second;
    }
    //打印请求信息
    LOG_INFO<<"请求信息:"<<req->getBody();

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
    LOG_INFO << "开始生成session_st";
    session_st session;
    session=chatSession::getInstance()->gennerateSessionstByReq(req);
    session=chatSession::getInstance()->createNewSessionOrUpdateSession(session);
    if(session.selectapi.empty())
    {
        //select api ....
        session.selectapi="chaynsapi";
    }
    std::string selectapi=session.selectapi;

    Json::Value responsejson;
    LOG_INFO << "发送请求给chaynsapi ";
    LOG_INFO << "session.curConversationId: " << session.curConversationId;
    LOG_INFO << "session.preConversationId: " << session.preConversationId;
    ApiManager::getInstance().getApiByApiName(selectapi)->postChatMessage(session);
    responsejson=session.responsemessage;

    Json::StreamWriterBuilder writer;
    writer["emitUTF8"] = true;  // 确保输出UTF-8编码
    std::string jsonStr = Json::writeString(writer, responsejson);
    LOG_DEBUG << "chaynsapi返回结果:" << jsonStr;

    // 获取responsejson中的message和statusCode
    //string message=Json::writeString(writer,responsejson["message"]);
    string message=responsejson["message"].asString();
    int statusCode=responsejson["statusCode"].asInt();

    // Check User-Agent to determine if the request is from Kilo Code
    std::string userAgent = req->getHeader("user-agent");
    if (userAgent.find("Kilo-Code") != std::string::npos) {
        // Kilo Code requires responses to be tool calls.
        // If the message from the backend is plain text, wrap it in <attempt_completion> tags.
        // A simple check to see if it already looks like a tool call (starts with '<').
        if (!message.empty() && message.front() != '<') {
            message = "<attempt_completion><result>" + message + "</result></attempt_completion>";
        }
    }

    const auto& stream = reqbody["stream"].asBool();

    LOG_INFO << "stream: " << stream;

    HttpResponsePtr resp;
    string oldConversationId=session.curConversationId;
    
    if(statusCode==200)
     {// ... existing code ...
         if(stream)
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
            auto shared_context = std::make_shared<StreamContext>(message);
            resp = HttpResponse::newStreamResponse([shared_context, oldConversationId](char *buffer, size_t maxBytes) -> size_t {
                try {
                    if (shared_context->sent_done) {
                        return 0;
                    }

                    if (time(nullptr) - shared_context->start_time > 60) {
                        LOG_WARN << "Stream timeout";
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
                    LOG_DEBUG << "Sent position: " << shared_context->pos << "/" << shared_context->response_message.length();
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    
                    return to_send;
                    
                } catch (const std::exception& e) {
                    LOG_ERROR << "Stream response error: " << e.what();
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
        }
        else
        {
                Json::Value response;
                LOG_INFO << "非流式响应";
                response["id"] = "chatcmpl-" + oldConversationId.substr(0,10);
                response["object"] = "chat.completion";
                response["created"] = static_cast<int>(time(nullptr));
                response["choices"][0]["message"]["content"] = message;
                response["choices"][0]["message"]["role"] = "assistant";
                response["choices"][0]["finish_reason"] = "stop";
                response["choices"][0]["index"] = 0;
                
                resp = HttpResponse::newHttpJsonResponse(response);
                resp->setStatusCode(k200OK);
                resp->setContentTypeString("application/json; charset=utf-8");
                
                // 先发送响应
                callback(resp);              
        }
        // 更新session,重新生成conversationId
        LOG_INFO << "更新session:";
        LOG_DEBUG << "session.curConversationId: " << session.curConversationId;
        LOG_DEBUG << "session.preConversationId: " << session.preConversationId;
        chatSession::getInstance()->coverSessionresponse(session);
        LOG_INFO << "更新session完成:";
        LOG_DEBUG << "session.curConversationId: " << session.curConversationId;
        LOG_DEBUG << "session.preConversationId: " << session.preConversationId;
        ApiManager::getInstance().getApiByApiName(selectapi)->afterResponseProcess(session);
    }
    else
        {   
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
    LOG_INFO << "chaynsapi/v1/models";
    Json::Value response= ApiManager::getInstance().getApiByApiName("chaynsapi")->getModels();
    auto resp = HttpResponse::newHttpJsonResponse(response);
    callback(resp);
}
void AiApi::accountAdd(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "accountAdd";
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

    LOG_INFO << "addAccount start";
    Json::Value response;
    list<Accountinfo_st> accountList;
    for(auto &item:*jsonPtr)
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
    });
    addAccountThread.detach();
    auto resp = HttpResponse::newHttpJsonResponse(response);
    callback(resp);
    LOG_INFO << "addAccount end";
}
void AiApi::accountInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "accountInfo";
    auto accountList=AccountManager::getInstance().getAccountList();
    Json::Value response;
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
            response.append(accountitem);
        }
    }
    auto resp = HttpResponse::newHttpJsonResponse(response);
    callback(resp);
}
void AiApi::accountDelete(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "accountDelete";
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
 
    Json::Value response;
    list<Accountinfo_st> accountList;
    for(auto &item:*jsonPtr)
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
    });
    deleteAccountThread.detach();   
    auto resp = HttpResponse::newHttpJsonResponse(response);
    callback(resp);
    //删除accountdbManager中的账号
}
void AiApi::accountDbInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "accountDbInfo";
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
        response.append(accountitem);
    }
    auto resp = HttpResponse::newHttpJsonResponse(response);
    callback(resp);
}
