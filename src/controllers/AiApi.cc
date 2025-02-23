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
#include <fstream>
using namespace drogon;
using namespace drogon::orm;

// Add definition of your processing function here
void AiApi::chaynsapichat(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO<<"=======================chaynsapichat 请求开始=======================";
    //打印请求头
    LOG_DEBUG<<"请求头:";
    for(auto &header : req->getHeaders())
    {
        LOG_DEBUG<<header.first<<":"<<header.second;
    }
    //打印请求信息
    
    std::string body = Json::FastWriter().write(*(req->getJsonObject()));
    LOG_DEBUG<<"请求信息:"<<body;

    
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
    auto startTimeInfo = std::chrono::system_clock::now();
    std::time_t startTimeT = std::chrono::system_clock::to_time_t(startTimeInfo);
    std::stringstream ssbegin;
    ssbegin << std::put_time(std::localtime(&startTimeT), "%c");
    LOG_INFO << "处理请求开始时间: " << ssbegin.str();


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
    LOG_INFO << "chaynsapi返回结果 statusCode: " << statusCode;
    LOG_INFO << "chaynsapi返回结果 message size: " << message.size();
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
                    bool sent_done = false;
                    std::string response_message;
                    time_t start_time;
                    bool first_chunk = true;  // 添加标记来追踪第一个chunk
                    StreamContext(const std::string& msg) : 
                    response_message(msg), 
                    start_time(time(nullptr)) {}
                };

            // 在lambda外创建上下文，并使用shared_ptr来共享
            auto shared_context = std::make_shared<StreamContext>(message);
            resp = HttpResponse::newStreamResponse([shared_context, oldConversationId](char *buffer, size_t maxBytes) -> size_t {
                try {
                    // 使用独立的状态管理器     
                   if (shared_context->sent_done) {
                        return 0;
                    }

                    if (time(nullptr) - shared_context->start_time > 60) {
                        LOG_WARN << "Stream timeout";
                        return 0;
                    }
                    
                    
                    
                    // 处理结束情况
                    if (shared_context->pos >= shared_context->response_message.length()) {
                        const std::string done_message = "data: [DONE]\n\n";
                        size_t done_size = std::min(done_message.length(), maxBytes);
                        memcpy(buffer, done_message.c_str(), done_size);
                        shared_context->sent_done = true;
                        return done_size;
                    }

                    // 计算当前chunk大小（确保不切断UTF-8字符）
                    size_t chunk_size = 0;
                    size_t remaining = shared_context->response_message.length() - shared_context->pos;
                    size_t target_size = std::min(remaining, size_t(9)); // 每次发送3个汉字
                    
                    while (chunk_size < target_size) {
                        unsigned char c = shared_context->response_message[shared_context->pos + chunk_size];
                        if ((c & 0x80) == 0) chunk_size += 1;
                        else if ((c & 0xE0) == 0xC0) chunk_size += 2;
                        else if ((c & 0xF0) == 0xE0) chunk_size += 3;
                        else if ((c & 0xF8) == 0xF0) chunk_size += 4;
                        if (chunk_size > target_size) break;
                    }

                    // 构造SSE消息
                    Json::Value data;
                    data["id"] = "chatcmpl-" + oldConversationId.substr(0, 5) + "-" + std::to_string(time(nullptr));
                    data["object"] = "chat.completion.chunk";
                    data["created"] = static_cast<int>(time(nullptr));
                    // 如果是第一个chunk，发送完整的开始部分
                    if (shared_context->first_chunk) {
                        data["choices"][0]["delta"]["content"] = 
                            shared_context->response_message.substr(0, chunk_size);
                        shared_context->first_chunk = false;
                    } else {
                        data["choices"][0]["delta"]["content"] = 
                            shared_context->response_message.substr(shared_context->pos, chunk_size);
                    }
                    data["choices"][0]["finish_reason"] = Json::Value();
                    data["choices"][0]["index"] = 0;
                    
                    Json::FastWriter writer;
                    std::string json_str = writer.write(data);
                    if (!json_str.empty() && json_str[json_str.length()-1] == '\n') {
                        json_str.pop_back();
                    }
                    
                    std::string chunk_str = "data: " + json_str + "\n\n";
                    size_t to_send = std::min(chunk_str.length(), maxBytes);
                    memcpy(buffer, chunk_str.c_str(), to_send);
                    
                    shared_context->pos += chunk_size;
                    LOG_DEBUG << "Sent position: " << shared_context->pos << "/" << shared_context->response_message.length();
                    // 控制发送速率
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
            resp->addHeader("Transfer-Encoding", "chunked");
            resp->addHeader("Keep-Alive", "timeout=120");
            resp->addHeader("Transfer-Encoding", "chunked");
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
        LOG_DEBUG << "PRE session.curConversationId: " << session.curConversationId;
        LOG_DEBUG << "PRE session.preConversationId: " << session.preConversationId;
        chatSession::getInstance()->coverSessionresponse(session);
        LOG_INFO << "更新session完成:";
        LOG_DEBUG << "CUR session.curConversationId: " << session.curConversationId;
        LOG_DEBUG << "CUR session.preConversationId: " << session.preConversationId;
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
    auto endtimeInfo = std::chrono::system_clock::now();
    std::time_t endtimeT = std::chrono::system_clock::to_time_t(endtimeInfo);
    std::stringstream ssendtime;
    ssendtime << std::put_time(std::localtime(&endtimeT), "%c");
    LOG_INFO << "消耗时间: " << std::chrono::duration_cast<std::chrono::seconds>(endtimeInfo - startTimeInfo).count() << "秒";
    LOG_INFO << "chaynsapichat 请求结束: " << ssendtime.str();
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
void AiApi::logsInfo(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    /*
    const std::string logPath = "../logs/aichat.log";
    std::ifstream logFile(logPath);

    if (!logFile.is_open()) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("无法打开日志文件");
        callback(resp);
        return;
    }

    int lines = 0;
    if (req->getParameter("lines") != "") {
        try {
            lines = std::stoi(req->getParameter("lines"));
        } catch (...) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k400BadRequest);
            resp->setBody("无效的 lines 参数");
            callback(resp);
            return;
        }
    }

    // 如果需要限制行数，先将所有行读入vector
    std::vector<std::string> allLines;
    std::string line;
    while (std::getline(logFile, line)) {
        allLines.push_back(line);
    }

    // 如果指定了行数限制，只保留最后N行
    if (lines > 0 && lines < allLines.size()) {
        allLines.erase(allLines.begin(), allLines.end() - lines);
    }

    std::stringstream formattedContent;
    formattedContent << R"(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>日志查看器</title>
    <style>
        body {
            margin: 0;
            padding: 20px;
            background-color: #f5f5f5;
            font-family: Arial, sans-serif;
        }
        .container {
            max-width: 1400px;
            margin: 0 auto;
            background-color: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }
        pre {
            white-space: pre-wrap;
            word-wrap: break-word;
            font-family: 'Consolas', monospace;
            font-size: 14px;
            line-height: 1.5;
            background-color: #f8f9fa;
            padding: 15px;
            border-radius: 4px;
            border: 1px solid #e9ecef;
            margin: 0;
        }
        .header {
            text-align: center;
            margin-bottom: 20px;
        }
        .header h1 {
            color: #333;
            margin: 0 0 10px 0;
        }
        .summary {
            margin-bottom: 20px;
            padding: 10px;
            background-color: #e9ecef;
            border-radius: 4px;
            color: #495057;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>系统日志查看器</h1>
        </div>
        <div class="summary">
            总日志行数: )" << allLines.size() << R"( 行
        </div>
        <pre>)";

    // 输出所有日志行
    for (const auto& logLine : allLines) {
        formattedContent << logLine << "\n";
    }

    formattedContent << "</pre></div></body></html>";

    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeCode(CT_TEXT_HTML);
    resp->addHeader("Content-Type", "text/html; charset=utf-8");
    resp->setBody(formattedContent.str());
    callback(resp);
    */
   const std::string logPath = "../logs/aichat.log";
    std::ifstream logFile(logPath);

    if (!logFile.is_open()) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("无法打开日志文件");
        callback(resp);
        return;
    }

    int lines = 0;
    if (req->getParameter("lines") != "") {
        try {
            lines = std::stoi(req->getParameter("lines"));
        } catch (...) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k400BadRequest);
            resp->setBody("无效的 lines 参数");
            callback(resp);
            return;
        }
    }

    std::vector<std::string> allLines;
    std::string line;
    while (std::getline(logFile, line)) {
        allLines.push_back(line);
    }

    if (lines > 0 && lines < allLines.size()) {
        allLines.erase(allLines.begin(), allLines.end() - lines);
    }

    std::stringstream formattedContent;
    formattedContent << "<!DOCTYPE html>\n"
        "<html lang=\"zh-CN\">\n"
        "<head>\n"
        "<meta charset=\"UTF-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "<title>系统日志查看器</title>\n"
        "<style>\n"
        "body { margin: 0; padding: 20px; background: #f5f5f5; font-family: monospace; }\n"
        ".container { max-width: 1600px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n"
        "pre { white-space: pre-wrap; word-wrap: break-word; font-size: 13px; line-height: 1.5; background: #1e1e1e; color: #d4d4d4; padding: 15px; border-radius: 4px; margin: 0; }\n"
        ".header { margin-bottom: 20px; color: #333; }\n"
        ".summary { margin-bottom: 15px; padding: 10px; background: #fff; border-radius: 4px; }\n"
        ".timestamp { color: #569cd6; }\n"
        ".level-DEBUG { color: #4ec9b0; }\n"
        ".level-INFO { color: #9cdcfe; }\n"
        ".level-ERROR { color: #f44747; }\n"
        ".level-WARNING { color: #ce9178; }\n"
        ".json { color: #ce9178; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<div class=\"container\">\n"
        "<div class=\"header\"><h1>系统日志查看器</h1></div>\n"
        "<div class=\"summary\">总行数: " << allLines.size() << "</div>\n"
        "<pre>\n";

    // 处理每一行日志
    for (const auto& log : allLines) {
        std::string processedLog = log;
        
        // 处理时间戳
        if (log.length() >= 26) {
            formattedContent << "<span class=\"timestamp\">" 
                           << processedLog.substr(0, 26) 
                           << "</span>";
            processedLog = processedLog.substr(26);
        }

        // 处理日志级别
        if (processedLog.find("DEBUG") != std::string::npos) {
            processedLog = std::regex_replace(processedLog, 
                std::regex("DEBUG"), 
                "<span class=\"level-DEBUG\">DEBUG</span>");
        } else if (processedLog.find("INFO") != std::string::npos) {
            processedLog = std::regex_replace(processedLog, 
                std::regex("INFO"), 
                "<span class=\"level-INFO\">INFO</span>");
        } else if (processedLog.find("ERROR") != std::string::npos) {
            processedLog = std::regex_replace(processedLog, 
                std::regex("ERROR"), 
                "<span class=\"level-ERROR\">ERROR</span>");
        }

        // 处理JSON内容
        if (processedLog.find("{") != std::string::npos && 
            processedLog.find("}") != std::string::npos) {
            size_t jsonStart = processedLog.find("{");
            size_t jsonEnd = processedLog.rfind("}") + 1;
            if (jsonStart != std::string::npos && jsonEnd != std::string::npos) {
                formattedContent << processedLog.substr(0, jsonStart)
                               << "<span class=\"json\">"
                               << processedLog.substr(jsonStart, jsonEnd - jsonStart)
                               << "</span>"
                               << processedLog.substr(jsonEnd);
            }
        } else {
            formattedContent << processedLog;
        }

        formattedContent << "\n";
    }

    formattedContent << "</pre>\n"
                     << "</div>\n"
                     << "</body>\n"
                     << "</html>";

    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeCode(CT_TEXT_HTML);
    resp->addHeader("Content-Type", "text/html; charset=utf-8");
    resp->setBody(formattedContent.str());
    callback(resp);
}
std::string AiApi::generateHtmlPage() {
    return R"(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>系统日志查看器</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            line-height: 1.6;
            margin: 0;
            padding: 20px;
            background-color: #f5f5f5;
        }
        .container {
            max-width: 1400px;
            margin: 0 auto;
            background-color: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }
        .day-section {
            margin-bottom: 30px;
        }
        .day-header {
            background: #4a90e2;
            color: white;
            padding: 10px 15px;
            border-radius: 4px;
            font-weight: bold;
            margin-bottom: 10px;
        }
        .log-entry {
            padding: 8px 15px;
            border-bottom: 1px solid #eee;
            font-family: 'Consolas', monospace;
            white-space: pre-wrap;
            word-wrap: break-word;
        }
        .log-entry:hover {
            background-color: #f8f8f8;
        }
        .log-time {
            color: #666;
            margin-right: 10px;
        }
        .log-level {
            font-weight: bold;
            padding: 2px 6px;
            border-radius: 3px;
            margin-right: 10px;
        }
        .level-DEBUG {
            background-color: #e8f5e9;
            color: #2e7d32;
        }
        .level-INFO {
            background-color: #e3f2fd;
            color: #1976d2;
        }
        .level-WARNING {
            background-color: #fff3e0;
            color: #f57c00;
        }
        .level-ERROR {
            background-color: #ffebee;
            color: #d32f2f;
        }
        .log-message {
            color: #333;
        }
        .log-json {
            background-color: #f8f9fa;
            padding: 10px;
            border-radius: 4px;
            margin: 5px 0;
        }
        .header {
            text-align: center;
            margin-bottom: 30px;
        }
        .header h1 {
            color: #333;
            margin-bottom: 10px;
        }
        .summary {
            color: #666;
            margin-bottom: 20px;
            padding: 10px;
            background-color: #f8f8f8;
            border-radius: 4px;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>系统日志查看器</h1>
        </div>
        <div class="summary">
            <strong>日志统计：</strong> <span id="log-count"></span>
        </div>
        <div id="log-content"></div>
    </div>
</body>
</html>
)";
}

LogEntry AiApi::parseLogLine(const std::string& log) {
    LogEntry entry;
    entry.original = log;

    // 解析时间戳和日志级别
    std::regex logPattern(R"((\d{8}\s+\d{2}:\d{2}:\d{2}\.\d+)\s+UTC\s+\d+\s+(\w+)\s+(.*))");
    std::smatch matches;
    
    if (std::regex_search(log, matches, logPattern)) {
        entry.timestamp = matches[1].str();
        entry.level = matches[2].str();
        entry.message = matches[3].str();
    }

    return entry;
}

std::string AiApi::formatLogEntry(const LogEntry& entry) {
    std::stringstream result;
    
    result << "<div class='log-entry'>"
           << "<span class='log-time'>" << entry.timestamp << "</span>"
           << "<span class='log-level level-" << entry.level << "'>" << entry.level << "</span>";

    // 检查是否包含JSON数据
    if (entry.message.find("{") != std::string::npos && entry.message.find("}") != std::string::npos) {
        result << "<div class='log-message'>" 
               << entry.message.substr(0, entry.message.find("{")) 
               << "<pre class='log-json'>" << entry.message.substr(entry.message.find("{")) << "</pre>"
               << "</div>";
    } else {
        result << "<span class='log-message'>" << entry.message << "</span>";
    }

    result << "</div>";
    return result.str();
}
