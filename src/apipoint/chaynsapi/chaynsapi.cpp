#include <drogon/drogon.h>
#include <chaynsapi.h>
#include <../../apiManager/Apicomn.h>
#include <unistd.h>
#include <chrono>
IMPLEMENT_RUNTIME(chaynsapi,chaynsapi);
using namespace drogon;

chaynsapi::chaynsapi()
{
   
}

chaynsapi::~chaynsapi()
{
}

void chaynsapi::init()
{
    loadModels();
}

// 上传图片到 image-service
std::string chaynsapi::uploadImageToService(const ImageInfo& image, const std::string& personId, const std::string& authToken)
{
    LOG_INFO << "Uploading image to image-service for personId: " << personId;
    
    // 如果已经有 URL，直接返回
    if (!image.uploadedUrl.empty()) {
        LOG_INFO << "Image already has URL: " << image.uploadedUrl;
        return image.uploadedUrl;
    }
    
    // 需要上传 base64 图片
    if (image.base64Data.empty()) {
        LOG_ERROR << "No image data to upload";
        return "";
    }
    
    auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
    
    // 构建上传请求
    // URL: POST https://cube.tobit.cloud/image-service/v3/Images/{personId}
    std::string uploadPath = "/image-service/v3/Images/" + personId;
    
    // 首先解码 base64 数据
    std::string decodedData = drogon::utils::base64Decode(image.base64Data);
    
    // 确定文件扩展名
    std::string extension = "png";
    if (image.mediaType.find("jpeg") != std::string::npos || image.mediaType.find("jpg") != std::string::npos) {
        extension = "jpg";
    } else if (image.mediaType.find("gif") != std::string::npos) {
        extension = "gif";
    } else if (image.mediaType.find("webp") != std::string::npos) {
        extension = "webp";
    }
    
    // 创建 HTTP 请求并构建 multipart/form-data 请求体
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(HttpMethod::Post);
    request->setPath(uploadPath);
    request->addHeader("Authorization", "Bearer " + authToken);
    
    // 构建 multipart/form-data 请求体
    std::string boundary = "----WebKitFormBoundary" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    std::string contentType = "multipart/form-data; boundary=" + boundary;
    request->setContentTypeString(contentType);
    
    std::string mimeType = "image/" + extension;
    if (extension == "jpg") mimeType = "image/jpeg";
    
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"image." + extension + "\"\r\n";
    body += "Content-Type: " + mimeType + "\r\n\r\n";
    body += decodedData;
    body += "\r\n--" + boundary + "--\r\n";
    
    request->setBody(body);
    
    auto [result, response] = client->sendRequest(request);
    
    if (result != ReqResult::Ok) {
        LOG_ERROR << "Failed to upload image: network error";
        return "";
    }
    
    if (response->statusCode() != k200OK && response->statusCode() != k201Created) {
        LOG_ERROR << "Failed to upload image: status " << response->statusCode() << " body: " << response->getBody();
        return "";
    }
    
    // 解析响应获取图片URL
    auto jsonResp = response->getJsonObject();
    if (!jsonResp) {
        LOG_ERROR << "Failed to parse upload response as JSON";
        return "";
    }
    
    // 响应格式: {"image": {"path": "xxx"}, "baseDomain": "https://tsimg.cloud/"}
    if (jsonResp->isMember("baseDomain") && jsonResp->isMember("image") && (*jsonResp)["image"].isMember("path")) {
        std::string baseDomain = (*jsonResp)["baseDomain"].asString();
        std::string imagePath = (*jsonResp)["image"]["path"].asString();
        std::string imageUrl = baseDomain + imagePath;
        LOG_INFO << "Image uploaded successfully: " << imageUrl;
        return imageUrl;
    }
    
    LOG_ERROR << "Unexpected upload response format";
    return "";
}

void chaynsapi::postChatMessage(session_st& session)
{
    LOG_INFO << "chaynsapi::postChatMessage";
    string modelname = session.selectmodel;
    
    shared_ptr<Accountinfo_st> accountinfo = nullptr;
    
    // 检查是否存在上下文 (ThreadId)，如果有则使用相同账户
    std::string savedAccountUserName;
    {
        std::lock_guard<std::mutex> lock(m_threadMapMutex);
        auto it = m_threadMap.find(session.curConversationId);
        if (it != m_threadMap.end() && !it->second.accountUserName.empty()) {
            savedAccountUserName = it->second.accountUserName;
            LOG_INFO << "Found saved account userName: " << savedAccountUserName << " for curConvId: " << session.curConversationId;
        }
    }
    
    if (!savedAccountUserName.empty()) {
        // 使用之前创建 thread 时的相同账户
        AccountManager::getInstance().getAccountByUserName("chaynsapi", savedAccountUserName, accountinfo);
        if (accountinfo == nullptr || !accountinfo->tokenStatus) {
            LOG_WARN << "Saved account " << savedAccountUserName << " is no longer valid, falling back to getAccount";
            AccountManager::getInstance().getAccount("chaynsapi", accountinfo, "pro");
        }
    } else {
        // 新会话，获取新账户
        AccountManager::getInstance().getAccount("chaynsapi", accountinfo, "pro");
    }
    
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
    
    // 处理图片上传
    std::vector<std::string> uploadedImageUrls;
    if (!session.requestImages.empty()) {
        LOG_INFO << "Processing " << session.requestImages.size() << " images for upload";
        for (auto& img : session.requestImages) {
            std::string imageUrl = uploadImageToService(img, accountinfo->personId, accountinfo->authToken);
            if (!imageUrl.empty()) {
                uploadedImageUrls.push_back(imageUrl);
                img.uploadedUrl = imageUrl; // 更新 session 中的 URL
            }
        }
        LOG_INFO << "Successfully uploaded " << uploadedImageUrls.size() << " images";
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
        
        // 添加图片数组 (如果有上传的图片)
        if (!uploadedImageUrls.empty()) {
            Json::Value imagesArray(Json::arrayValue);
            for (const auto& url : uploadedImageUrls) {
                Json::Value imgObj;
                imgObj["url"] = url;
                imagesArray.append(imgObj);
            }
            messageBody["images"] = imagesArray;
            LOG_INFO << "Added " << uploadedImageUrls.size() << " images to follow-up message";
        }

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
        
        // 添加图片数组到消息 (如果有上传的图片)
        if (!uploadedImageUrls.empty()) {
            Json::Value imagesArray(Json::arrayValue);
            for (const auto& url : uploadedImageUrls) {
                Json::Value imgObj;
                imgObj["url"] = url;
                imagesArray.append(imgObj);
            }
            message["images"] = imagesArray;
            LOG_INFO << "Added " << uploadedImageUrls.size() << " images to new thread message";
        }
        
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
        ctx.accountUserName = accountinfo->userName; // 保存创建thread时使用的账户
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
void chaynsapi::checkAlivableTokens()
{

}
bool chaynsapi::checkAlivableToken(string token)
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
void chaynsapi::checkModels()
{

}
void chaynsapi::loadModels()
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
    
    LOG_INFO << "chayns NativeModelChatbot models successfully loaded: " << modelInfoMap.size() << " models.";
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
void chaynsapi::transferThreadContext(const std::string& oldId, const std::string& newId)
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
void chaynsapi::afterResponseProcess(session_st& session)
{
   // No longer needed with the new stateless API
}
void chaynsapi::eraseChatinfoMap(string ConversationId)
{
    std::lock_guard<std::mutex> lock(m_threadMapMutex);
    const auto erased = m_threadMap.erase(ConversationId);
    LOG_INFO << "eraseChatinfoMap: convId erased=" << erased;
}
Json::Value chaynsapi::getModels()
{
   return model_info_openai_format;
}
void* chaynsapi::createApi()
{
    chaynsapi* api=new chaynsapi();
    api->init();
    return api;
}
