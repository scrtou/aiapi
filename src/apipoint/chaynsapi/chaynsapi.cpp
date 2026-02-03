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
    LOG_INFO << "[chaynsAPI] 正在上传图片到图片服务, personId: " << personId;
    
    // 如果已经有 URL，直接返回
    if (!image.uploadedUrl.empty()) {
        LOG_INFO << "[chaynsAPI] 图片已有URL: " << image.uploadedUrl;
        return image.uploadedUrl;
    }
    
    // 需要上传 base64 图片
    if (image.base64Data.empty()) {
        LOG_ERROR << "[chaynsAPI] 没有图片数据可上传";
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
        LOG_ERROR << "[chaynsAPI] 上传图片失败: 网络错误";
        return "";
    }
    
    if (response->statusCode() != k200OK && response->statusCode() != k201Created) {
        LOG_ERROR << "[chaynsAPI] 上传图片失败: 状态码 " << response->statusCode() << " 响应: " << response->getBody();
        return "";
    }
    
    // 解析响应获取图片URL
    auto jsonResp = response->getJsonObject();
    if (!jsonResp) {
        LOG_ERROR << "[chaynsAPI] 解析上传响应JSON失败";
        return "";
    }
    
    // 响应格式: {"image": {"path": "xxx"}, "baseDomain": "https://tsimg.cloud/"}
    if (jsonResp->isMember("baseDomain") && jsonResp->isMember("image") && (*jsonResp)["image"].isMember("path")) {
        std::string baseDomain = (*jsonResp)["baseDomain"].asString();
        std::string imagePath = (*jsonResp)["image"]["path"].asString();
        std::string imageUrl = baseDomain + imagePath;
        LOG_INFO << "[chaynsAPI] 图片上传成功: " << imageUrl;
        return imageUrl;
    }
    
    LOG_ERROR << "[chaynsAPI] 上传响应格式异常";
    return "";
}

void chaynsapi::postChatMessage(session_st& session)
{
    LOG_INFO << "[chaynsAPI] 发送聊天消息";
    string modelname = session.selectmodel;
    
    shared_ptr<Accountinfo_st> accountinfo = nullptr;
    
    // 检查是否存在上下文 (ThreadId)，如果有则使用相同账户
    // 优化：只有当 is_continuation 为 true 时才查找 m_threadMap
    // 新会话直接创建新 ThreadContext，避免无效查找
    std::string savedAccountUserName;
    if (session.is_continuation && !session.prev_provider_key.empty()) {
        std::lock_guard<std::mutex> lock(m_threadMapMutex);
        // 使用 prev_provider_key 查找，因为它指向上一轮的会话ID
        auto it = m_threadMap.find(session.prev_provider_key);
        if (it != m_threadMap.end() && !it->second.accountUserName.empty()) {
            savedAccountUserName = it->second.accountUserName;
            LOG_INFO << "[chaynsAPI] 找到已保存的账户用户名: " << savedAccountUserName
                     << " (prev_provider_key: " << session.prev_provider_key << ")";
        }
    }
    
    if (!savedAccountUserName.empty()) {
        // 使用之前创建 thread 时的相同账户
        AccountManager::getInstance().getAccountByUserName("chaynsapi", savedAccountUserName, accountinfo);
        if (accountinfo == nullptr || !accountinfo->tokenStatus) {
            LOG_WARN << "[chaynsAPI] 已保存账户 " << savedAccountUserName << " 不再有效, 回退到获取新账户";
            AccountManager::getInstance().getAccount("chaynsapi", accountinfo, "pro");
        }
    } else {
        // 新会话，获取新账户
        AccountManager::getInstance().getAccount("chaynsapi", accountinfo, "pro");
    }
    
    if (accountinfo == nullptr || !accountinfo->tokenStatus)
    {
        LOG_ERROR << "[chaynsAPI] 获取有效账户失败";
        session.responsemessage["error"] = "No valid account available";
        session.responsemessage["statusCode"] = 500;
        return;
    }

    if (accountinfo->personId.empty()) {
        LOG_INFO << "[chaynsAPI] personId为空, 正在尝试获取";
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
                    LOG_INFO << "[chaynsAPI] 成功获取personId: " << accountinfo->personId;
                } else {
                    LOG_ERROR << "[chaynsAPI] 用户设置响应JSON中未找到personId, 响应: " << response->getBody();
                }
            } else {
                LOG_ERROR << "[chaynsAPI] 解析用户设置响应为JSON对象失败, 响应: " << response->getBody();
            }
        } else {
            LOG_ERROR << "[chaynsAPI] 获取用户设置失败, 状态码: " << (response ? response->statusCode() : 0) << ", 响应: " << (response ? std::string(response->getBody()) : "无响应");
        }
    }

    if (accountinfo->personId.empty()) {
        LOG_ERROR << "[chaynsAPI] 尝试获取后personId仍为空, 中止操作";
        session.responsemessage["error"] = "Failed to obtain a valid personId";
        session.responsemessage["statusCode"] = 500;
        return;
    }
    
    // 处理图片上传
    std::vector<std::string> uploadedImageUrls;
    if (!session.requestImages.empty()) {
        LOG_INFO << "[chaynsAPI] 正在处理 " << session.requestImages.size() << " 张图片上传";
        for (auto& img : session.requestImages) {
            std::string imageUrl = uploadImageToService(img, accountinfo->personId, accountinfo->authToken);
            if (!imageUrl.empty()) {
                uploadedImageUrls.push_back(imageUrl);
                img.uploadedUrl = imageUrl; // 更新 session 中的 URL
            }
        }
        LOG_INFO << "[chaynsAPI] 成功上传 " << uploadedImageUrls.size() << " 张图片";
    }
    
    auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
    string threadId;
    string userAuthorId; // 这是Bot在这个线程里的ID
    string lastMessageTime;

    // 3. 检查是否存在上下文 (ThreadId)
    // 优化：只有当 is_continuation 为 true 时才查找 m_threadMap
    // 新会话直接创建新 ThreadContext，避免无效查找
    bool isFollowUp = false;
    if (session.is_continuation && !session.prev_provider_key.empty()) {
        std::lock_guard<std::mutex> lock(m_threadMapMutex);
        // 使用 prev_provider_key 查找，因为它指向上一轮的会话ID
        // 转移逻辑在发送响应给客户端后进行（在 updateResponseSession/coverSessionresponse 中）
        auto it = m_threadMap.find(session.prev_provider_key);
        if (it != m_threadMap.end()) {
            threadId = it->second.threadId;
            userAuthorId = it->second.userAuthorId;
            isFollowUp = true;
            LOG_INFO << "[chaynsAPI] 找到现有threadId: " << threadId
                     << " (prev_provider_key: " << session.prev_provider_key << ")";
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
        
        /*
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
        */
        messageBody["text"] = messageText;
        LOG_DEBUG << "发送的消息" << messageText;
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
            LOG_INFO << "[chaynsAPI] 已添加 " << uploadedImageUrls.size() << " 张图片到后续消息";
        }

        auto reqSend = HttpRequest::newHttpJsonRequest(messageBody);
        reqSend->setMethod(HttpMethod::Post);
        string path = "/intercom-backend/v2/thread/" + threadId + "/message";
        reqSend->setPath(path);
        reqSend->addHeader("Authorization", "Bearer " + accountinfo->authToken);
        
        LOG_INFO << "[chaynsAPI] 正在发送后续消息到线程: " << threadId;
        
        auto sendResult = client->sendRequest(reqSend);
        if (sendResult.first != ReqResult::Ok) {
            LOG_ERROR << "[chaynsAPI] 发送后续消息失败";
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
            LOG_ERROR << "[chaynsAPI] 后续消息发送失败, 状态码: " << responseSend->statusCode() << ", 响应体: " << responseSend->getBody();
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
        LOG_INFO << "[chaynsAPI] 正在创建新线程: 注入系统提示词 (" << session.systemprompt.length() << " 字符)";
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
        // NOTE: Do not overwrite `full_message` here. We intentionally include the
        // system prompt (and optional history) when creating a new thread.
        
        Json::Value sendMessageRequest;
        Json::Value member1;
        member1["isAdmin"] = true;
        member1["personId"] = accountinfo->personId;
        sendMessageRequest["members"].append(member1);

        Json::Value member2;
        const auto& model_info = modelInfoMap[modelname];
        if (!model_info.isMember("personId") || !model_info["personId"].isString()) {
            LOG_ERROR << "[chaynsAPI] 模型personId缺失: " << modelname;
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
        LOG_DEBUG<<"发送的消息"<<full_message;

        // 添加图片数组到消息 (如果有上传的图片)
        if (!uploadedImageUrls.empty()) {
            Json::Value imagesArray(Json::arrayValue);
            for (const auto& url : uploadedImageUrls) {
                Json::Value imgObj;
                imgObj["url"] = url;
                imagesArray.append(imgObj);
            }
            message["images"] = imagesArray;
            LOG_INFO << "[chaynsAPI] 已添加 " << uploadedImageUrls.size() << " 张图片到新线程消息";
        }
        
        sendMessageRequest["messages"].append(message);

        auto reqSend = HttpRequest::newHttpJsonRequest(sendMessageRequest);
        reqSend->setMethod(HttpMethod::Post);
        reqSend->setPath("/intercom-backend/v2/thread?forceCreate=true");
        reqSend->addHeader("Authorization", "Bearer " + accountinfo->authToken);
        
        LOG_INFO << "[chaynsAPI] 正在创建新线程";

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
            LOG_ERROR << "[chaynsAPI] 创建线程失败, 状态码: " << responseSend->statusCode();
            session.responsemessage["error"] = "Failed to create thread";
            session.responsemessage["statusCode"] = responseSend->statusCode();
            return;
        }
    }

    if (threadId.empty() || lastMessageTime.empty()) {
        LOG_ERROR << "[chaynsAPI] 关键信息缺失: threadId或lastMessageTime";
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

    // 6. 轮询获取结果
    string response_message;
    int response_statusCode = 204;
    int pollCount = 0;

    // 注意：这里需要根据 lastMessageTime 轮询
    string pollPath = "/intercom-backend/v2/thread/" + threadId + "/message?take=1000&afterDate=" + lastMessageTime;
    LOG_INFO << "[chaynsAPI] 开始轮询, 最大重试次数: " << MAX_RETRIES << ", URL: " << pollPath;
    
    for (int retry = 0; retry < MAX_RETRIES; ++retry) {
        pollCount++;
        auto reqGet = HttpRequest::newHttpRequest();
        reqGet->setMethod(HttpMethod::Get);
        reqGet->setPath(pollPath);
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
                        LOG_INFO << "[chaynsAPI] 轮询结束, 总计轮询 " << pollCount << " 次, 成功获取响应";
                        LOG_INFO << "[chaynsAPI] 回复内容 " << response_message;

                        goto found;
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY));
    }
    
    LOG_INFO << "[chaynsAPI] 轮询结束, 总计轮询 " << pollCount << " 次, 未获取到响应";

 found:
    if (response_statusCode == 200) {
        session.responsemessage["message"] = response_message;
    } else {
        LOG_ERROR << "[chaynsAPI] 等待线程响应超时: " << threadId;
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
    LOG_INFO << "[chaynsAPI] 验证Token响应: " << response->getStatusCode();
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
        LOG_ERROR << "[chaynsAPI] 从API获取模型列表失败";
        return;
    }
    
    Json::Value api_models;
    Json::Reader reader;
    if (!reader.parse(string(response->getBody()), api_models)) {
        LOG_ERROR << "[chaynsAPI] 解析模型API响应失败";
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
    
    LOG_INFO << "[chaynsAPI] chayns NativeModelChatbot模型加载成功: " << modelInfoMap.size() << " 个模型";
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
    LOG_INFO << "[chaynsAPI] 正在尝试转移线程上下文, 从 " << oldId << " 到 " << newId;
    std::lock_guard<std::mutex> lock(m_threadMapMutex);
    auto it = m_threadMap.find(oldId);
    if (it != m_threadMap.end()) {
        m_threadMap[newId] = it->second;
        m_threadMap.erase(it);
        LOG_INFO << "[chaynsAPI] 成功转移线程上下文, 从 " << oldId << " 到 " << newId;
    }
    else
    {
        LOG_WARN << "[chaynsAPI] 转移线程上下文失败: oldId " << oldId << " 在threadMap中未找到";
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
    LOG_INFO << "[chaynsAPI] 删除会话映射: convId 删除数量=" << erased;
}
Json::Value chaynsapi::getModels()
{
    loadModels();
   return model_info_openai_format;
}
void* chaynsapi::createApi()
{
    chaynsapi* api=new chaynsapi();
    api->init();
    return api;
}
