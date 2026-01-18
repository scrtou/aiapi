#include <drogon/drogon.h>
#include <chaynsapi.h>
#include <../../apiManager/Apicomn.h> 
#include <unistd.h>
IMPLEMENT_RUNTIME(chaynsapi,Chaynsapi);
using namespace drogon;

Chaynsapi::Chaynsapi()
{
   
}

Chaynsapi::~Chaynsapi()
{
}

void Chaynsapi::init()
{
    loadModels();
}

void Chaynsapi::postChatMessage(session_st& session)
{
    LOG_INFO << "Chaynsapi::postChatMessage";
    string modelname = session.selectmodel;
    
    shared_ptr<Accountinfo_st> accountinfo = nullptr;
    AccountManager::getInstance().getAccount("chaynsapi", accountinfo, "pro");
    if (accountinfo == nullptr || !accountinfo->tokenStatus)
    {
        LOG_ERROR << "Failed to get a valid account for chaynsapi";
        session.responsemessage["error"] = "No valid account available";
        session.responsemessage["statusCode"] = 500;
        return;
    }

    if (accountinfo->personId.empty()) {
        LOG_INFO << "personId is empty, attempting to fetch it.";
        auto client = HttpClient::newHttpClient("https://auth.chayns.net");
        auto request = HttpRequest::newHttpRequest();
        request->setMethod(HttpMethod::Get);
        request->setPath("/v2/userSettings");
        request->addHeader("Authorization", "Bearer " + accountinfo->authToken);
        auto [result, response] = client->sendRequest(request);
        if (result == ReqResult::Ok && response->statusCode() == k200OK) {
            auto jsonResp = response->getJsonObject();
            if (jsonResp) {
                if (jsonResp->isMember("personId")) {
                    accountinfo->personId = (*jsonResp)["personId"].asString();
                    LOG_INFO << "Successfully fetched personId: " << accountinfo->personId;
                } else {
                    LOG_ERROR << "personId not found in userSettings response JSON. Body: " << response->getBody();
                }
            } else {
                LOG_ERROR << "Failed to parse userSettings response as JSON object. Body: " << response->getBody();
            }
        } else {
            LOG_ERROR << "Failed to fetch userSettings. Status code: " << (response ? response->statusCode() : 0) << ", Body: " << (response ? std::string(response->getBody()) : "No response");
        }
    }

    if (accountinfo->personId.empty()) {
        LOG_ERROR << "personId is still empty after attempting to fetch. Aborting.";
        session.responsemessage["error"] = "Failed to obtain a valid personId";
        session.responsemessage["statusCode"] = 500;
        return;
    }
    
    auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
    string threadId;
    string userAuthorId; // 这是Bot在这个线程里的ID
    string lastMessageTime;

    // 3. 检查是否存在上下文 (ThreadId)
    bool isFollowUp = false;
    {
        std::lock_guard<std::mutex> lock(m_threadMapMutex);
        // 使用 preConversationId 查找，因为 session 的 preConversationId 指向上一轮的 curConversationId
        // 使用 curConversationId 查找，因为 transferThreadContext 会把上下文转移到这个新的 ID 上
        auto it = m_threadMap.find(session.curConversationId);
        if (it != m_threadMap.end()) {
            threadId = it->second.threadId;
            userAuthorId = it->second.userAuthorId;
            isFollowUp = true;
            LOG_INFO << "Found existing threadId: " << threadId << " for curConvId: " << session.curConversationId;
        }
    }

    Json::Value sendResponseJson; // 保存发送请求的响应，用于提取时间和ID

    // 4. 发送请求 (分支逻辑)
    if (isFollowUp) {
        // =================================================
        // 分支 A: 后续对话 (发送消息到现有 Thread)
        // URL: /intercom-backend/v2/thread/{threadId}/message
        // =================================================
        Json::Value messageBody;
        string messageText = session.requestmessage;

        // [修改点]: 获取 client_type 并根据具体值注入提醒
        string clientType = session.client_info.get("client_type", "").asString();
        
        if (clientType == "Kilo-Code" ) {
            LOG_INFO << "为 " << clientType << " 客户端注入 System Reminder...";
            // 这是一个强力的 Reminder，包含所有可用工具的列表
            string reminder = 
                "\n\n(SYSTEM REMINDER: Continue the task immediately. Do NOT introduce yourself. "
                "You have access to the following XML tools: "
                "<read_file>, <search_files>, <list_files>, "
                "<apply_diff>, <write_to_file>, <delete_file>, "
                "<execute_command>, <ask_followup_question>, <attempt_completion>, "
                "<switch_mode>, <new_task>, <update_todo_list>, <fetch_instructions>. "
                "You MUST use these exact XML tags to perform actions. "
                "Do NOT use markdown code blocks for the XML tools.)";

            messageText += reminder;
        }

        messageBody["text"] = messageText;
        messageBody["cursorPosition"] = messageText.size(); 

        auto reqSend = HttpRequest::newHttpJsonRequest(messageBody);
        reqSend->setMethod(HttpMethod::Post);
        string path = "/intercom-backend/v2/thread/" + threadId + "/message";
        reqSend->setPath(path);
        reqSend->addHeader("Authorization", "Bearer " + accountinfo->authToken);
        
        LOG_INFO << "Sending follow-up message to thread: " << threadId;
        
        auto sendResult = client->sendRequest(reqSend);
        if (sendResult.first != ReqResult::Ok) {
            LOG_ERROR << "Failed to send follow-up message";
            session.responsemessage["error"] = "Failed to send message";
            session.responsemessage["statusCode"] = 500;
            return;
        }
        
        auto responseSend = sendResult.second;
        if (responseSend->statusCode() == k200OK || responseSend->statusCode() == k201Created) {
            // 解析响应，更新 lastMessageTime
            sendResponseJson = *responseSend->getJsonObject();
            if (sendResponseJson.isMember("creationTime")) {
                lastMessageTime = sendResponseJson["creationTime"].asString();
            }
            // 确保 userAuthorId 正确 (响应中的 author.id 即为发送者ID)
            if (sendResponseJson.isMember("author") && sendResponseJson["author"].isMember("id")) {
                userAuthorId = sendResponseJson["author"]["id"].asString();
            }
        } else {
            LOG_ERROR << "Follow-up failed code: " << responseSend->statusCode() << " body: " << responseSend->getBody();
            // 如果后续发送失败（例如线程已关闭），可能需要降级为创建新线程，这里简单处理为报错
            session.responsemessage["error"] = "Failed to append message";
            session.responsemessage["statusCode"] = responseSend->statusCode();
            return;
        }
    } else {
        // =================================================
        // 分支 B: 新对话 (创建新 Thread)
        // URL: /intercom-backend/v2/thread?forceCreate=true
        // =================================================
        LOG_INFO << "Creating New Thread: Injecting System Prompt (" << session.systemprompt.length() << " chars)";
        string full_message;
        if(!session.message_context.empty())
        {
            full_message=session.systemprompt + "\n""接下来，我会发给你openai接口格式的历史消息，：\n";
            full_message = full_message+session.message_context.toStyledString();
            full_message = full_message+"\n用户现在的问题是:\n"+session.requestmessage;
        }
        else
        {
            full_message =session.systemprompt +"\n"+ session.requestmessage;
        }

        
        Json::Value sendMessageRequest;
        Json::Value member1;
        member1["isAdmin"] = true;
        member1["personId"] = accountinfo->personId;
        sendMessageRequest["members"].append(member1);

        Json::Value member2;
        const auto& model_info = modelInfoMap[modelname];
        if (!model_info.isMember("personId") || !model_info["personId"].isString()) {
            LOG_ERROR << "Model personId missing: " << modelname;
            session.responsemessage["error"] = "Model config error";
            session.responsemessage["statusCode"] = 500;
            return;
        }
        member2["personId"] = model_info["personId"].asString();
        sendMessageRequest["members"].append(member2);
        sendMessageRequest["nerMode"] = "None";
        sendMessageRequest["priority"] = 0;
        sendMessageRequest["typeId"] = 8;
        
        Json::Value message;
        message["text"] = full_message;
        sendMessageRequest["messages"].append(message);

        auto reqSend = HttpRequest::newHttpJsonRequest(sendMessageRequest);
        reqSend->setMethod(HttpMethod::Post);
        reqSend->setPath("/intercom-backend/v2/thread?forceCreate=true");
        reqSend->addHeader("Authorization", "Bearer " + accountinfo->authToken);
        
        LOG_INFO << "Creating new thread";

        auto sendResult = client->sendRequest(reqSend);
        if (sendResult.first != ReqResult::Ok) {
             session.responsemessage["error"] = "Failed to connect";
             session.responsemessage["statusCode"] = 500;
             return;
        }
        auto responseSend = sendResult.second;
        
        if (responseSend->statusCode() == k200OK || responseSend->statusCode() == k201Created) {
            sendResponseJson = *responseSend->getJsonObject();
            if (sendResponseJson.isMember("id")) {
                threadId = sendResponseJson["id"].asString();
                
                // 提取 userAuthorId (Bot 在群里的 ID)
                if (sendResponseJson.isMember("members") && sendResponseJson["members"].isArray()) {
                    for (const auto& member : sendResponseJson["members"]) {
                        if (member.isMember("personId") && member["personId"].asString() == accountinfo->personId) {
                            if (member.isMember("id") && member["id"].isString()) {
                                userAuthorId = member["id"].asString();
                            }
                            break;
                        }
                    }
                }
                
                // 提取时间
                if (sendResponseJson.isMember("messages") && sendResponseJson["messages"].isArray() && sendResponseJson["messages"].size() > 0) {
                     lastMessageTime = sendResponseJson["messages"][0]["creationTime"].asString();
                }
            }
        } else {
            LOG_ERROR << "Create thread failed: " << responseSend->statusCode();
            session.responsemessage["error"] = "Failed to create thread";
            session.responsemessage["statusCode"] = responseSend->statusCode();
            return;
        }
    }

    if (threadId.empty() || lastMessageTime.empty()) {
        LOG_ERROR << "Critical info missing: threadId or lastMessageTime";
        session.responsemessage["error"] = "Protocol error";
        session.responsemessage["statusCode"] = 500;
        return;
    }

    // 5. 更新上下文映射表
    // 将当前的 curConversationId 指向这个 threadId，以便下一次请求（带上这个cur作为pre）能找到
    {
        std::lock_guard<std::mutex> lock(m_threadMapMutex);
        ThreadContext ctx;
        ctx.threadId = threadId;
        ctx.userAuthorId = userAuthorId;
        m_threadMap[session.curConversationId] = ctx;
    }

    // 6. 轮询获取结果 (逻辑保持不变)
    string response_message;
    int response_statusCode = 204; 

    // 注意：这里需要根据 lastMessageTime 轮询
    for (int retry = 0; retry < MAX_RETRIES; ++retry) {
        auto reqGet = HttpRequest::newHttpRequest();
        reqGet->setMethod(HttpMethod::Get);
        string getPath = "/intercom-backend/v2/thread/" + threadId + "/message?take=1000&afterDate=" + lastMessageTime;
        LOG_INFO << "Polling URL: " << getPath;
        reqGet->setPath(getPath);
        reqGet->addHeader("Authorization", "Bearer " + accountinfo->authToken);

        auto getResult = client->sendRequest(reqGet);
        if (getResult.first != ReqResult::Ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY));
            continue;
        }
        
        auto responseGet = getResult.second;
        if (responseGet->statusCode() == k200OK) {
            auto jsonResp = responseGet->getJsonObject();
            if (jsonResp && jsonResp->isArray() && !jsonResp->empty()) {
                // 查找最新消息
                for (int i = jsonResp->size() - 1; i >= 0; --i) {
                    const auto& message = (*jsonResp)[i];
                    // 过滤条件：不是 Bot 发的 (author.id != userAuthorId) 且 typeId == 1 (文本)
                    if (message.isMember("author") && message["author"].isMember("id") &&
                        message["author"]["id"].asString() != userAuthorId &&
                        message.isMember("typeId") && message["typeId"].asInt() == 1) {
                        
                        if (message.isMember("text") && message["text"].isString()) {
                            response_message = message["text"].asString();
                        }
                        response_statusCode = 200;
                        goto found;
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY));
    }

 found:
    if (response_statusCode == 200) {
        session.responsemessage["message"] = response_message;
    } else {
        LOG_ERROR << "Timeout waiting for response in thread " << threadId;
        session.responsemessage["error"] = "Timeout waiting for response";
    }
    session.responsemessage["statusCode"] = response_statusCode;
}
void Chaynsapi::checkAlivableTokens()
{

}
bool Chaynsapi::checkAlivableToken(string token)
{
    auto client = HttpClient::newHttpClient("https://auth.chayns.net");
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(HttpMethod::Get);
    request->setPath("/v2/userSettings");
    request->addHeader("Authorization", "Bearer " + token);
    auto [result, response] = client->sendRequest(request);
    LOG_DEBUG << "checkAlivableToken response: " << response->getStatusCode();
    if(response->getStatusCode()!=200)
    {
        return false;
    }
    return true;
}
void Chaynsapi::checkModels()
{

}
void Chaynsapi::loadModels()
{
    auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
    auto request = HttpRequest::newHttpRequest();
    
    request->setMethod(HttpMethod::Get);
    request->setPath("/chayns-ai-chatbot/nativeModelChatbot");
    
    auto [result, response] = client->sendRequest(request);
    
    if (result != ReqResult::Ok || response->statusCode() != k200OK) {
        LOG_ERROR << "Failed to fetch models from API";
        return;
    }
    
    Json::Value api_models;
    Json::Reader reader;
    if (!reader.parse(string(response->getBody()), api_models)) {
        LOG_ERROR << "Failed to parse models API response";
        return;
    }
    
    model_info_openai_format["object"] = "list";
    model_info_openai_format["data"] = Json::Value(Json::arrayValue);

    for (const auto& model : api_models) {
        string showName = model.get("showName", "").asString();
        if (!showName.empty()) {
            modelInfoMap[showName] = model;

            Json::Value tmp_model_info;
            tmp_model_info["id"] = showName;
            tmp_model_info["object"] = "model";
            tmp_model_info["created"] = 1626777600;
            tmp_model_info["owned_by"] = "chayns";
            model_info_openai_format["data"].append(tmp_model_info);
        }
    }
    
    LOG_INFO << "Chayns NativeModelChatbot models successfully loaded: " << modelInfoMap.size() << " models.";
}

std::string generateGuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++) {
        ss << dis(gen);
    }
    ss << "-";
    for (int i = 0; i < 4; i++) {
        ss << dis(gen);
    }
    ss << "-4";  // Version 4
    for (int i = 0; i < 3; i++) {
        ss << dis(gen);
    }
    ss << "-";
    ss << dis2(gen);  // Variant
    for (int i = 0; i < 3; i++) {
        ss << dis(gen);
    }
    ss << "-";
    for (int i = 0; i < 12; i++) {
        ss << dis(gen);
    }
    return ss.str();
}
void Chaynsapi::transferThreadContext(const std::string& oldId, const std::string& newId)
{
    LOG_INFO << "Attempting to transfer thread context from " << oldId << " to " << newId;
    std::lock_guard<std::mutex> lock(m_threadMapMutex);
    auto it = m_threadMap.find(oldId);
    if (it != m_threadMap.end()) {
        m_threadMap[newId] = it->second;
        m_threadMap.erase(it);
        LOG_INFO << "Successfully transferred thread context from " << oldId << " to " << newId;
    }
    else
    {
        LOG_WARN << "Failed to transfer thread context: oldId " << oldId << " not found in threadMap.";
    }
}
void Chaynsapi::afterResponseProcess(session_st& session)
{
   // No longer needed with the new stateless API
}
void Chaynsapi::eraseChatinfoMap(string ConversationId)
{
    std::lock_guard<std::mutex> lock(m_threadMapMutex);
    const auto erased = m_threadMap.erase(ConversationId);
    LOG_INFO << "eraseChatinfoMap: convId erased=" << erased;
}
Json::Value Chaynsapi::getModels()
{
   return model_info_openai_format;
}
void* Chaynsapi::createApi()
{
    Chaynsapi* chaynsapi=new Chaynsapi();
    chaynsapi->init();
    return chaynsapi;
}
