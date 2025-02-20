#include "AiApi.h"
#include <drogon/HttpResponse.h>
#include <json/json.h>
#include <unistd.h>
#include <apiManager/ApiManager.h>
#include <sessionManager/Session.h>
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
            resp->addHeader("Keep-Alive", "timeout=60");
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