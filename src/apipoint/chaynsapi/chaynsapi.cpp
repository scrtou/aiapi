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

    // 从配置 custom_config.upstream_error_texts 加载上游错误文本列表
    auto& customConfig = drogon::app().getCustomConfig();
    if (customConfig.isMember("upstream_error_texts") && customConfig["upstream_error_texts"].isArray()) {
        for (const auto& item : customConfig["upstream_error_texts"]) {
            if (item.isString()) {
                m_upstreamErrorTexts.push_back(item.asString());
            }
        }
        LOG_INFO << "[chaynsAPI] 已从配置加载" << m_upstreamErrorTexts.size() << " 条上游错误文本";
    } else {
        LOG_WARN << "[chaynsAPI] 配置中未找到 upstream_error_texts，上游错误文本匹配将不可用";
    }
}


std::string chaynsapi::uploadImageToService(const ImageInfo& image, const std::string& personId, const std::string& authToken)
{
    LOG_INFO << "[chaynsAPI] 正在上传图片到图片服务，personId：" << personId;
    
    // 如果已经有 URL，直接返回
    if (!image.uploadedUrl.empty()) {
        LOG_INFO << "[chaynsAPI] 图片已有URL：" << image.uploadedUrl;
        return image.uploadedUrl;
    }
    
    // 需要上传 图片
    if (image.base64Data.empty()) {
        LOG_ERROR << "[chaynsAPI] 没有图片数据可上传";
        return "";
    }
    
    auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
    
    // 构建上传请求

    std::string uploadPath = "/image-service/v3/Images/" + personId;
    
    // 首先解码 数据
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
    
    // 创建 HTTP 请求并构建 / 请求体
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(HttpMethod::Post);
    request->setPath(uploadPath);
    request->addHeader("Authorization", "Bearer " + authToken);
    

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
        LOG_ERROR << "[chaynsAPI] 上传图片失败： 网络错误";
        return "";
    }
    
    if (response->statusCode() != k200OK && response->statusCode() != k201Created) {
        LOG_ERROR << "[chaynsAPI] 上传图片失败： 状态码" << response->statusCode() << " 响应: " << response->getBody();
        return "";
    }
    
    // 解析响应获取图片URL
    auto jsonResp = response->getJsonObject();
    if (!jsonResp) {
        LOG_ERROR << "[chaynsAPI] 解析上传响应JSON失败";
        return "";
    }
    

    if (jsonResp->isMember("baseDomain") && jsonResp->isMember("image") && (*jsonResp)["image"].isMember("path")) {
        std::string baseDomain = (*jsonResp)["baseDomain"].asString();
        std::string imagePath = (*jsonResp)["image"]["path"].asString();
        std::string imageUrl = baseDomain + imagePath;
        LOG_INFO << "[chaynsAPI] 图片上传成功：" << imageUrl;
        return imageUrl;
    }
    
    LOG_ERROR << "[chaynsAPI] 上传响应格式异常";
    return "";
}

provider::ProviderResult chaynsapi::generate(session_st& session)
{
    postChatMessage(session);

    provider::ProviderResult result;
    result.text = session.response.message.get("message", "").asString();
    result.statusCode = session.response.message.get("statusCode", 500).asInt();

    if (result.statusCode == 200) {
        result.error = provider::ProviderError::none();
    } else {
        result.error = provider::ProviderError{
            provider::ProviderErrorCode::InternalError,
            "Provider returned error",
            "",
            result.statusCode
        };
    }

    return result;
}

void chaynsapi::postChatMessage(session_st& session)
{
    LOG_INFO << "[chaynsAPI] 发送聊天消息";
    string modelname = session.request.model;
    
    // ========== 上游重试外层循环 ==========
    // 重试策略（三层）:
    // 内层： 同一线程上重复询问 SAME_线程_RETRIES 次
    // 中层： 同一账号创建新线程重试（consecutiveFails < CONSECUTIVE_FAILS_BEFORE_SWITCH）
    // 外层： 切换账号重试（consecutiveFails >= CONSECUTIVE_FAILS_BEFORE_SWITCH）
    // totalAttempts： 外层总重试计数，达到 MAX_UPSTREAM_RETRIES 时放弃
    int totalAttempts = 0;
    int consecutiveFails = 0;  // 跨线程的连续失败计数，用于判断是否需要换账号
    bool upstreamSuccess = false;
    
    // 最终结果保存
    string final_response_message;
    int final_response_statusCode = 204;
    string final_threadId;
    string final_userAuthorId;
    string final_accountUserName;
    
    // 上传的图片URL（在首次尝试时上传，后续重试复用）
    std::vector<std::string> uploadedImageUrls;
    bool imagesUploaded = false;
    
    while (totalAttempts < MAX_UPSTREAM_RETRIES && !upstreamSuccess) {
        totalAttempts++;
        bool needSwitchAccount = (consecutiveFails >= CONSECUTIVE_FAILS_BEFORE_SWITCH);
        
        if (totalAttempts > 1) {
            LOG_INFO << "[chaynsAPI] 上游重试第" << totalAttempts << " 次 (连续失败: " << consecutiveFails 
                     << ", 需要换账号: " << (needSwitchAccount ? "是" : "否") << ")";
        }
        
        // ---- 1. 获取账号 ----
        shared_ptr<Accountinfo_st> accountinfo = nullptr;
        
        // 首次尝试时，检查是否有已保存的账户用于继续会话
        std::string savedAccountUserName;
        if (totalAttempts == 1 && !needSwitchAccount) {
            if (session.state.isContinuation && !session.provider.prevProviderKey.empty()) {
                std::lock_guard<std::mutex> lock(m_threadMapMutex);
                auto it = m_threadMap.find(session.provider.prevProviderKey);
                if (it != m_threadMap.end() && !it->second.accountUserName.empty()) {
                    savedAccountUserName = it->second.accountUserName;
                    LOG_INFO << "[chaynsAPI] 找到已保存的账户用户名：" << savedAccountUserName
                             << " (prevProviderKey: " << session.provider.prevProviderKey << ")";
                }
            }
        }
        
        if (!savedAccountUserName.empty() && !needSwitchAccount) {
            AccountManager::getInstance().getAccountByUserName("chaynsapi", savedAccountUserName, accountinfo);
            if (accountinfo == nullptr || !accountinfo->tokenStatus) {
                LOG_WARN << "[chaynsAPI] 已保存账户" << savedAccountUserName << " 不再有效, 回退到获取新账户";
                AccountManager::getInstance().getAccount("chaynsapi", accountinfo, "pro");
            }
        } else {
            // 新会话或需要换账号，获取新账户
            AccountManager::getInstance().getAccount("chaynsapi", accountinfo, "pro");
        }
        
        if (accountinfo == nullptr || !accountinfo->tokenStatus) {
            LOG_ERROR << "[chaynsAPI] 获取有效账户失败";
            if (totalAttempts >= MAX_UPSTREAM_RETRIES) {
                session.response.message["error"] = "No valid account available";
                session.response.message["statusCode"] = 500;
                return;
            }
            consecutiveFails++;
            std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY * 5));
            continue;
        }
        

        if (accountinfo->personId.empty()) {
            LOG_INFO << "[chaynsAPI] personId为空，正在尝试获取";
            auto authClient = HttpClient::newHttpClient("https://auth.chayns.net");
            auto request = HttpRequest::newHttpRequest();
            request->setMethod(HttpMethod::Get);
            request->setPath("/v2/userSettings");
            request->addHeader("Authorization", "Bearer " + accountinfo->authToken);
            auto [result, response] = authClient->sendRequest(request);
            if (result == ReqResult::Ok && response->statusCode() == k200OK) {
                auto jsonResp = response->getJsonObject();
                if (jsonResp) {
                    if (jsonResp->isMember("personId")) {
                        accountinfo->personId = (*jsonResp)["personId"].asString();
                        LOG_INFO << "[chaynsAPI] 成功获取personId：" << accountinfo->personId;
                    } else {
                        LOG_ERROR << "[chaynsAPI] 用户设置响应JSON中未找到personId，响应：" << response->getBody();
                    }
                } else {
                    LOG_ERROR << "[chaynsAPI] 解析用户设置响应为JSON对象失败，响应：" << response->getBody();
                }
            } else {
                LOG_ERROR << "[chaynsAPI] 获取用户设置失败，状态码：" << (response ? response->statusCode() : 0) << ", 响应: " << (response ? std::string(response->getBody()) : "无响应");
            }
        }
        
        if (accountinfo->personId.empty()) {
            LOG_ERROR << "[chaynsAPI] 尝试获取后personId仍为空，中止当前尝试";
            consecutiveFails++;
            if (totalAttempts >= MAX_UPSTREAM_RETRIES) {
                session.response.message["error"] = "Failed to obtain a valid personId";
                session.response.message["statusCode"] = 500;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY * 5));
            continue;
        }
        
        // ---- 3. 处理图片上传 (仅首次) ----
        if (!imagesUploaded && !session.request.images.empty()) {
            LOG_INFO << "[chaynsAPI] 正在处理" << session.request.images.size() << " 张图片上传";
            for (auto& img : session.request.images) {
                std::string imageUrl = uploadImageToService(img, accountinfo->personId, accountinfo->authToken);
                if (!imageUrl.empty()) {
                    uploadedImageUrls.push_back(imageUrl);
                    img.uploadedUrl = imageUrl;
                }
            }
            LOG_INFO << "[chaynsAPI] 成功上传" << uploadedImageUrls.size() << " 张图片";
            imagesUploaded = true;
        }
        
        // ---- 4. 发送请求（首次发送） ----
        auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
        string threadId;
        string userAuthorId;
        string lastMessageTime;
        
        // 只在首次尝试且未要求换账号时，尝试使用已有线程
        bool isFollowUp = false;
        if (totalAttempts == 1 && !needSwitchAccount && session.state.isContinuation && !session.provider.prevProviderKey.empty()) {
            std::lock_guard<std::mutex> lock(m_threadMapMutex);
            auto it = m_threadMap.find(session.provider.prevProviderKey);
            if (it != m_threadMap.end()) {
                threadId = it->second.threadId;
                userAuthorId = it->second.userAuthorId;
                isFollowUp = true;
                LOG_INFO << "[chaynsAPI] 找到现有线程Id：" << threadId
                         << " (prevProviderKey: " << session.provider.prevProviderKey << ")";
            }
        }
        
        Json::Value sendResponseJson;
        bool sendFailed = false;
        
        if (isFollowUp) {
            // =================================================
            // 分支 A： 后续对话 (发送消息到现有 线程)
            // =================================================
            Json::Value messageBody;
            string messageText = session.request.message;
            
            messageBody["text"] = messageText;
            LOG_DEBUG << "发送的消息" << messageText;
            messageBody["cursorPosition"] = messageText.size();
            
            if (!uploadedImageUrls.empty()) {
                Json::Value imagesArray(Json::arrayValue);
                for (const auto& url : uploadedImageUrls) {
                    Json::Value imgObj;
                    imgObj["url"] = url;
                    imagesArray.append(imgObj);
                }
                messageBody["images"] = imagesArray;
                LOG_INFO << "[chaynsAPI] 已添加" << uploadedImageUrls.size() << " 张图片到后续消息";
            }
            
            auto reqSend = HttpRequest::newHttpJsonRequest(messageBody);
            reqSend->setMethod(HttpMethod::Post);
            string path = "/intercom-backend/v2/thread/" + threadId + "/message";
            reqSend->setPath(path);
            reqSend->addHeader("Authorization", "Bearer " + accountinfo->authToken);
            
            LOG_INFO << "[chaynsAPI] 正在发送后续消息到线程：" << threadId;
            
            auto sendResult = client->sendRequest(reqSend);
            if (sendResult.first != ReqResult::Ok) {
                LOG_ERROR << "[chaynsAPI] 发送后续消息失败(网络错误)";
                sendFailed = true;
            } else {
                auto responseSend = sendResult.second;
                if (responseSend->statusCode() == k200OK || responseSend->statusCode() == k201Created) {
                    sendResponseJson = *responseSend->getJsonObject();
                    if (sendResponseJson.isMember("creationTime")) {
                        lastMessageTime = sendResponseJson["creationTime"].asString();
                    }
                    if (sendResponseJson.isMember("author") && sendResponseJson["author"].isMember("id")) {
                        userAuthorId = sendResponseJson["author"]["id"].asString();
                    }
                } else {
                    LOG_ERROR << "[chaynsAPI] 后续消息发送失败，状态码：" << responseSend->statusCode() << ", 响应体: " << responseSend->getBody();
                    sendFailed = true;
                }
            }
        } else {
            // =================================================
            // 分支 B： 新对话 (创建新 线程)
            // =================================================
            LOG_INFO << "[chaynsAPI] 正在创建新线程： 注入系统提示词 (" << session.request.systemPrompt.length() << " 字符)";
            string full_message;
            
            if(!session.provider.messageContext.empty())
            {
                full_message=session.request.systemPrompt + "\n""接下来，我会发给你openai接口格式的历史消息，：\n";
                full_message = full_message+session.provider.messageContext.toStyledString();
                full_message = full_message+"\n用户现在的问题是:\n"+session.request.message;
            }
            else
            {
                full_message =session.request.systemPrompt +"\n"+ session.request.message;
            }
            
            Json::Value sendMessageRequest;
            Json::Value member1;
            member1["isAdmin"] = true;
            member1["personId"] = accountinfo->personId;
            sendMessageRequest["members"].append(member1);
            
            Json::Value member2;
            const auto& model_info = modelInfoMap[modelname];
            if (!model_info.isMember("personId") || !model_info["personId"].isString()) {
                LOG_ERROR << "[chaynsAPI] 模型personId缺失：" << modelname;
                session.response.message["error"] = "Model config error";
                session.response.message["statusCode"] = 500;
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
            
            if (!uploadedImageUrls.empty()) {
                Json::Value imagesArray(Json::arrayValue);
                for (const auto& url : uploadedImageUrls) {
                    Json::Value imgObj;
                    imgObj["url"] = url;
                    imagesArray.append(imgObj);
                }
                message["images"] = imagesArray;
                LOG_INFO << "[chaynsAPI] 已添加" << uploadedImageUrls.size() << " 张图片到新线程消息";
            }
            
            sendMessageRequest["messages"].append(message);
            
            auto reqSend = HttpRequest::newHttpJsonRequest(sendMessageRequest);
            reqSend->setMethod(HttpMethod::Post);
            reqSend->setPath("/intercom-backend/v2/thread?forceCreate=true");
            reqSend->addHeader("Authorization", "Bearer " + accountinfo->authToken);
            
            LOG_INFO << "[chaynsAPI] 正在创建新线程";
            
            auto sendResult = client->sendRequest(reqSend);
            if (sendResult.first != ReqResult::Ok) {
                LOG_ERROR << "[chaynsAPI] 创建线程失败(网络错误)";
                sendFailed = true;
            } else {
                auto responseSend = sendResult.second;
                if (responseSend->statusCode() == k200OK || responseSend->statusCode() == k201Created) {
                    sendResponseJson = *responseSend->getJsonObject();
                    if (sendResponseJson.isMember("id")) {
                        threadId = sendResponseJson["id"].asString();
                        
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
                        
                        if (sendResponseJson.isMember("messages") && sendResponseJson["messages"].isArray() && sendResponseJson["messages"].size() > 0) {
                            lastMessageTime = sendResponseJson["messages"][0]["creationTime"].asString();
                        }
                    }
                } else {
                    LOG_ERROR << "[chaynsAPI] 创建线程失败，状态码：" << responseSend->statusCode();
                    sendFailed = true;
                }
            }
        }
        
        // 如果首次发送就失败了（网络错误等），直接进入外层重试
        if (sendFailed) {
            consecutiveFails++;
            LOG_WARN << "[chaynsAPI] 发送请求失败，连续失败次数：" << consecutiveFails;
            if (consecutiveFails >= CONSECUTIVE_FAILS_BEFORE_SWITCH) {
                LOG_WARN << "[chaynsAPI] 连续失败" << consecutiveFails << " 次, 下次将切换账号";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY * 5));
            continue;
        }
        
        if (threadId.empty() || lastMessageTime.empty()) {
            LOG_ERROR << "[chaynsAPI] 关键信息缺失： 线程Id或lastMessageTime";
            consecutiveFails++;
            if (consecutiveFails >= CONSECUTIVE_FAILS_BEFORE_SWITCH) {
                LOG_WARN << "[chaynsAPI] 连续失败" << consecutiveFails << " 次, 下次将切换账号";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY * 5));
            continue;
        }
        
        // ========== 5. 同线程重试内层循环 ==========
        // 在同一线程上进行 SAME_线程_RETRIES 次尝试：
        //   第1次: 已经发送了消息，只需轮询结果
        //   第2~n次: 在同一线程上重新发送消息，然后轮询结果
        bool sameThreadSuccess = false;
        
        for (int sameThreadAttempt = 1; sameThreadAttempt <= SAME_THREAD_RETRIES; ++sameThreadAttempt) {
            
            // 第2次及以后: 需要在同一线程上重新发送消息
            if (sameThreadAttempt > 1) {
                LOG_INFO << "[chaynsAPI] 同线程重试第" << sameThreadAttempt << "/" << SAME_THREAD_RETRIES 
                         << " 次 (threadId: " << threadId << ")";
                
                // 重新发送消息到同一线程
                Json::Value retryMessageBody;
                retryMessageBody["text"] = session.request.message;
                retryMessageBody["cursorPosition"] = (int)session.request.message.size();
                
                if (!uploadedImageUrls.empty()) {
                    Json::Value imagesArray(Json::arrayValue);
                    for (const auto& url : uploadedImageUrls) {
                        Json::Value imgObj;
                        imgObj["url"] = url;
                        imagesArray.append(imgObj);
                    }
                    retryMessageBody["images"] = imagesArray;
                }
                
                auto reqRetry = HttpRequest::newHttpJsonRequest(retryMessageBody);
                reqRetry->setMethod(HttpMethod::Post);
                string retryPath = "/intercom-backend/v2/thread/" + threadId + "/message";
                reqRetry->setPath(retryPath);
                reqRetry->addHeader("Authorization", "Bearer " + accountinfo->authToken);
                
                LOG_INFO << "[chaynsAPI] 正在重新发送消息到线程：" << threadId;
                
                auto retryResult = client->sendRequest(reqRetry);
                if (retryResult.first != ReqResult::Ok) {
                    LOG_ERROR << "[chaynsAPI] 同线程重试发送失败(网络错误)";
                    continue; // 尝试下一次同线程重试
                }
                
                auto retryResponse = retryResult.second;
                if (retryResponse->statusCode() != k200OK && retryResponse->statusCode() != k201Created) {
                    LOG_ERROR << "[chaynsAPI] 同线程重试发送失败，状态码：" << retryResponse->statusCode();
                    continue; // 尝试下一次同线程重试
                }
                

                auto retryJson = retryResponse->getJsonObject();
                if (retryJson && retryJson->isMember("creationTime")) {
                    lastMessageTime = (*retryJson)["creationTime"].asString();
                }
                if (retryJson && retryJson->isMember("author") && (*retryJson)["author"].isMember("id")) {
                    userAuthorId = (*retryJson)["author"]["id"].asString();
                }
                
                // 添加短暂延迟
                std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY * 5));
            }
            
            // ---- 轮询获取结果 ----
            string response_message;
            int response_statusCode = 204;
            int pollCount = 0;
            bool pollFound = false;
            
            string pollPath = "/intercom-backend/v2/thread/" + threadId + "/message?take=1000&afterDate=" + lastMessageTime;
            LOG_INFO << "[chaynsAPI] 开始轮询 (同线程第" << sameThreadAttempt << " 次), 最大轮询次数: " << MAX_RETRIES;
            
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
                        for (int i = jsonResp->size() - 1; i >= 0; --i) {
                            const auto& msg = (*jsonResp)[i];
                            if (msg.isMember("author") && msg["author"].isMember("id") &&
                                msg["author"]["id"].asString() != userAuthorId &&
                                msg.isMember("typeId") && msg["typeId"].asInt() == 1) {
                                
                                if (msg.isMember("text") && msg["text"].isString()) {
                                    response_message = msg["text"].asString();
                                }
                                response_statusCode = 200;
                                pollFound = true;
                                LOG_INFO << "[chaynsAPI] 轮询结束，总计轮询" << pollCount << " 次, 成功获取响应";
                                LOG_INFO << "[chaynsAPI] 回复内容" << response_message;
                                break;
                            }
                        }
                        if (pollFound) break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY));
            }
            
            if (!pollFound) {
                LOG_INFO << "[chaynsAPI] 轮询结束，总计轮询" << pollCount << " 次, 未获取到响应";
            }
            
            // ---- 检查上游响应是否为错误 ----
            bool isUpstreamError = false;
            
            if (response_statusCode != 200) {
                isUpstreamError = true;
                LOG_WARN << "[chaynsAPI] 上游错误： 轮询超时未获取响应 (线程Id：" << threadId 
                         << ", 同线程第 " << sameThreadAttempt << "/" << SAME_THREAD_RETRIES << " 次)";
            } else {
                for (const auto& errorText : m_upstreamErrorTexts) {
                    if (response_message == errorText) {
                        isUpstreamError = true;
                        LOG_WARN << "[chaynsAPI] 上游错误： 收到错误文本 '" << errorText << "' (线程Id：" << threadId
                                 << ", 同线程第 " << sameThreadAttempt << "/" << SAME_THREAD_RETRIES << " 次)";
                        break;
                    }
                }
            }
            
            if (!isUpstreamError) {
                // 上游成功！
                sameThreadSuccess = true;
                upstreamSuccess = true;
                final_response_message = response_message;
                final_response_statusCode = response_statusCode;
                final_threadId = threadId;
                final_userAuthorId = userAuthorId;
                final_accountUserName = accountinfo->userName;
                LOG_INFO << "[chaynsAPI] 上游请求成功 (外层第" << totalAttempts << " 次, 同线程第 " << sameThreadAttempt << " 次)";
                break; // 退出同线程重试循环
            }
            
            // 同线程重试失败，如果还有重试机会则在同线程上重新发送
            if (sameThreadAttempt < SAME_THREAD_RETRIES) {
                LOG_INFO << "[chaynsAPI] 同线程重试： 将在同一线程上重新发送消息";
                std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY * 10));
            }
            
        } // 同线程重试循环结束
        
        // 如果同线程重试成功，退出外层循环
        if (upstreamSuccess) {
            break;
        }
        
        // 同线程所有重试都失败了
        consecutiveFails++;
        LOG_WARN << "[chaynsAPI] 同线程" << SAME_THREAD_RETRIES << " 次重试均失败, 连续失败次数: " << consecutiveFails 
                 << ", 总尝试次数: " << totalAttempts << "/" << MAX_UPSTREAM_RETRIES;
        
        if (consecutiveFails >= CONSECUTIVE_FAILS_BEFORE_SWITCH) {
            LOG_WARN << "[chaynsAPI] 连续失败" << consecutiveFails << " 次, 下次将切换账号并创建新会话";
        } else {
            LOG_INFO << "[chaynsAPI] 下次将使用同一账号创建新线程重试";
        }
        
        // 添加延迟避免过于频繁的重试
        std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY * 10));
        
    } // 外层重试循环结束（totalAttempts < MAX_UPSTREAM_RETRIES）
    
    // ========== 重试循环结束，处理最终结果 ==========
    
    if (upstreamSuccess) {
        // 更新上下文映射表
        {
            std::lock_guard<std::mutex> lock(m_threadMapMutex);
            ThreadContext ctx;
            ctx.threadId = final_threadId;
            ctx.userAuthorId = final_userAuthorId;
            ctx.accountUserName = final_accountUserName;
            m_threadMap[session.state.conversationId] = ctx;
        }
        
        session.response.message["message"] = final_response_message;
        session.response.message["statusCode"] = final_response_statusCode;
    } else {
        LOG_ERROR << "[chaynsAPI] 所有上游重试均失败 (总尝试次数：" << totalAttempts 
                 << "/" << MAX_UPSTREAM_RETRIES << ")";
        session.response.message["error"] = "Upstream failed after all retries";
        session.response.message["statusCode"] = 500;
    }
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
    LOG_INFO << "[chaynsAPI] 验证Token响应：" << response->getStatusCode();
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
    
    LOG_INFO << "[chaynsAPI] Native模型Chatbot模型加载成功：" << modelInfoMap.size() << " 个模型";
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
    ss << "-4";  // UUID 版本位：固定为 V4
    for (int i = 0; i < 3; i++) {
        ss << dis(gen);
    }
    ss << "-";
    ss << dis2(gen);  // UUID 变体位
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
    LOG_INFO << "[chaynsAPI] 正在尝试转移线程上下文，从" << oldId << " 到 " << newId;
    std::lock_guard<std::mutex> lock(m_threadMapMutex);
    auto it = m_threadMap.find(oldId);
    if (it != m_threadMap.end()) {
        m_threadMap[newId] = it->second;
        m_threadMap.erase(it);
        LOG_INFO << "[chaynsAPI] 成功转移线程上下文，从" << oldId << " 到 " << newId;
    }
    else
    {
        LOG_WARN << "[chaynsAPI] 转移线程上下文失败： oldId" << oldId << "在线程Map中未找到";
    }
}
void chaynsapi::afterResponseProcess(session_st& session)
{

}
void chaynsapi::eraseChatinfoMap(string ConversationId)
{
    std::lock_guard<std::mutex> lock(m_threadMapMutex);
    const auto erased = m_threadMap.erase(ConversationId);
    LOG_INFO << "[chaynsAPI] 删除会话映射： convId 删除数量=" << erased;
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
