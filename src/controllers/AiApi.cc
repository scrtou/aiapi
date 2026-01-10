#include "AiApi.h"
#include <drogon/HttpResponse.h>
#include <json/json.h>
#include <drogon/drogon.h>
#include <unistd.h>
#include <apiManager/ApiManager.h>
#include <accountManager/accountManager.h>
#include <dbManager/account/accountDbManager.h>
#include <channelManager/channelManager.h>
#include <sessionManager/Session.h>
#include <drogon/orm/Exception.h>
#include <drogon/orm/DbClient.h>
#include <vector> // 添加 vector 头文件
#include <ctime>
#include <iomanip>
#include <sstream>

using namespace drogon;
using namespace drogon::orm;

// Add definition of your processing function here
void AiApi::chaynsapichat(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    //打印所有的请求头
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
    string message=responsejson["message"].asString();
    LOG_INFO<<"回复message:"<<message;
    int statusCode=responsejson["statusCode"].asInt();
    string clientType = session.client_info.get("client_type", "").asString();
   // =========================================================
    // [修正] Kilo/Roo Code 客户端的清洗与纠错逻辑 (通用版)
    // =========================================================
    if ((clientType == "Kilo-Codetest") && !message.empty()) {
        LOG_INFO << "正在对 " << clientType << " 客户端的响应进行标签清洗...";
        
        auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
            if(from.empty()) return;
            size_t start_pos = 0;
            while((start_pos = str.find(from, start_pos)) != std::string::npos) {
                str.replace(start_pos, from.length(), to);
                start_pos += to.length();
            }
        };

        // 1. 基础标签纠错 (修正模型常见的拼写错误)
        replaceAll(message, "<write_file>", "<write_to_file>");
        replaceAll(message, "</write_file>", "</write_to_file>");
        replaceAll(message, "<list_dir>", "<list_files>");
        replaceAll(message, "</list_dir>", "</list_files>");
        replaceAll(message, "<run_command>", "<execute_command>");
        replaceAll(message, "</run_command>", "</execute_command>");

        // 定义所有合法的工具列表
        static const std::vector<std::string> kiloTools = {
             "read_file", "write_to_file", "execute_command", "search_files",
             "list_files", "attempt_completion", "ask_followup_question",
             "switch_mode", "new_task", "update_todo_list", "fetch_instructions",
             "apply_diff", "delete_file"
        };

        // 2. [通用修复] 检测 attempt_completion 包裹其他工具调用的情况
        // 如果 message 包含 attempt_completion，我们检查内部是否嵌套了其他工具
        if (message.find("<attempt_completion>") != std::string::npos) {
            for (const auto& tool : kiloTools) {
                // 跳过 attempt_completion 自身，否则会把自己剥离掉
                if (tool == "attempt_completion") continue;

                std::string openTag = "<" + tool + ">";
                std::string closeTag = "</" + tool + ">";

                // 如果在 attempt_completion 内部发现了其他工具标签
                if (message.find(openTag) != std::string::npos) {
                    LOG_INFO << "检测到 attempt_completion 错误包裹了工具: " << tool << "，正在剥离外层...";
                    
                    size_t start = message.find(openTag);
                    size_t end = message.rfind(closeTag);
                    
                    if (start != std::string::npos && end != std::string::npos) {
                        end += closeTag.length(); 
                        if (end > start) {
                            // 提取内部工具，丢弃外层的 attempt_completion
                            message = message.substr(start, end - start);
                            LOG_INFO << "剥离完成，工具指令已提取。";
                            // 找到一个就退出，因为 Kilo 一次只执行一个工具
                            goto skip_markdown_check; 
                        }
                    }
                }
            }
        }

        // 3. 兼容性提取逻辑 (处理 markdown 包裹 ```xml ... ```)
        // 只有当经过上面的清洗后，message 里依然没有可以直接执行的 tag 时，才尝试去剥离 markdown
        {
            bool hasDirectTool = false;
            for (const auto& tool : kiloTools) {
                // 检查是否已经是以 <tool> 开头（或包含裸露的 tag）
                if (message.find("<" + tool + ">") != std::string::npos) {
                    hasDirectTool = true;
                    break;
                }
            }

            if (hasDirectTool) {
                // 如果包含工具标签，检查是否被 Markdown 代码块包裹
                size_t startTag = message.find('<');
                size_t endTag = message.rfind('>');
                // 如果 < 出现在比较靠后的位置（说明前面有 ```xml 或者废话），则提取
                if (startTag != std::string::npos && endTag != std::string::npos && endTag > startTag) {
                     // 简单的启发式：如果不是从 0 开始，或者结尾后面还有东西，就裁剪
                     if (startTag > 0 || endTag < message.length() - 1) {
                        message = message.substr(startTag, endTag - startTag + 1);
                     }
                }
            } else {
                // 如果完全没有工具标签，说明是纯文本回复，强制包裹 attempt_completion 以便客户端显示
                // 防止 message 为空时包裹空标签
                if (!message.empty()) {
                    message = "<attempt_completion><result>" + message + "</result></attempt_completion>";
                }
            }
        }

        skip_markdown_check:;
    }
    // =========================================================

    const auto& stream = reqbody["stream"].asBool();

    LOG_INFO << "stream: " << stream;

    HttpResponsePtr resp;
    string oldConversationId=session.curConversationId;
    
    if(statusCode==200)
     {
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
            // 注意：这里使用的是经过处理（可能被清洗或包裹）后的 message
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
                    
                    //std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    
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
                // 使用处理后的 message
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
        // 注意：这里我们应该保存原始的回复到历史记录，还是处理过的？
        // 建议：保存处理过的 XML，这样上下文更干净，有助于模型理解
        session.responsemessage["message"] = message; 

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

    LOG_INFO << "addAccount start";
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
        accountitem["createtime"]=account.createTime;
        response.append(accountitem);
    }
    auto resp = HttpResponse::newHttpJsonResponse(response);
    callback(resp);
}

// 渠道管理接口实现
void AiApi::channelAdd(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "channelAdd";
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
        
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
        LOG_INFO << "channelAdd end";
    } catch (const std::exception& e) {
        LOG_ERROR << "channelAdd error: " << e.what();
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
    LOG_INFO << "channelInfo";
    
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
            response.append(channelItem);
        }
        
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
    } catch (const std::exception& e) {
        LOG_ERROR << "channelInfo error: " << e.what();
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
    LOG_INFO << "channelDelete";
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
        
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
    } catch (const std::exception& e) {
        LOG_ERROR << "channelDelete error: " << e.what();
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
    LOG_INFO << "channelUpdate";
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

// 更新数据库
        if (ChannelManager::getInstance().updateChannel(channelInfo)) {
            response["status"] = "success";
            response["message"] = "Channel updated successfully";
            response["id"] = channelInfo.id;
        } else {
            response["status"] = "failed";
            response["message"] = "Failed to update channel";
        }

auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
        LOG_INFO << "channelUpdate end";
    } catch (const std::exception& e) {
        LOG_ERROR << "channelUpdate error: " << e.what();
        Json::Value error;
        error["error"]["message"] = std::string("Database error: ") + e.what();
        error["error"]["type"] = "database_error";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(HttpStatusCode::k500InternalServerError);
        callback(resp);
    }
}
