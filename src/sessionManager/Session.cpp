#include "Session.h"
#include <time.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include "chaynsapi.h"
#include <apiManager/ApiManager.h>
#include <apiManager/Apicomn.h>
#include <tools/ZeroWidthEncoder.h>
#include <random>
#include <chrono>
using namespace drogon;
chatSession *chatSession::instance = nullptr;

chatSession::chatSession()
{
}

chatSession::~chatSession()
{
}

void chatSession::addSession(const std::string &ConversationId,session_st &session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    session_map[ConversationId] = session;
}

void chatSession::delSession(const std::string &ConversationId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    session_map.erase(ConversationId);
}

void chatSession::getSession(const std::string &ConversationId, session_st &session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = session_map.find(ConversationId);
    if (it == session_map.end())
    {
        LOG_WARN << "[会话管理] getSession: 未找到会话ID";
        return;
    }
    session = it->second;
}

void chatSession::updateSession(const std::string &ConversationId,session_st &session)
{
    if(session_map.find(ConversationId) != session_map.end())
    {    std::lock_guard<std::mutex> lock(mutex_);
        session_map[ConversationId] = session;
    }
}

// ========== 会话创建/更新辅助方法（消除重复代码）==========

void chatSession::updateExistingSessionFromRequest(const std::string& sessionId, session_st& session)
{
    // IMPORTANT:
    // When updating an existing session, we must merge *request-scoped* fields
    // (model/system/tools/current input/client info) into the stored session.
    // Otherwise fields like `tools` will be lost, and tool-bridge logic will
    // never see the client-provided tool definitions on follow-up turns.
    auto &stored = session_map[sessionId];

    // Request-scoped fields (always take the latest request values)
    stored.selectapi = session.selectapi;
    stored.selectmodel = session.selectmodel;
    if (!session.systemprompt.empty()) {
        stored.systemprompt = session.systemprompt;
    }
    stored.client_info = session.client_info;
    stored.requestmessage = session.requestmessage;
    stored.requestmessage_raw = session.requestmessage_raw.empty() ? session.requestmessage : session.requestmessage_raw;
    stored.requestImages = session.requestImages;
    if (!session.tools.isNull() && session.tools.isArray() && session.tools.size() > 0) {
        stored.tools = session.tools;
        stored.tools_raw = session.tools;  // 更新原始工具定义
    } else if (!session.tools_raw.isNull() && session.tools_raw.isArray() && session.tools_raw.size() > 0) {
        // 如果本次请求未携带 tools，保留旧的 tools_raw（用于 tool bridge 兜底）
        stored.tools_raw = session.tools_raw;
    }
    if (!session.toolChoice.empty()) {
        stored.toolChoice = session.toolChoice;
    }

    // Keep protocol flags in sync (mainly for Response API reuse of this helper)
    // IMPORTANT: for Response API follow-ups, `session.response_id` is per-request (new response id),
    // while `stored.response_id` should remain stable and match the session_map key (the previous response id).
    // So we MUST NOT overwrite stored.response_id here.
    const std::string incomingResponseId = session.response_id;
    stored.is_response_api = session.is_response_api;
    stored.has_previous_response_id = session.has_previous_response_id;
    if (stored.response_id.empty() && !incomingResponseId.empty()) {
        // only initialize if missing; do not mutate existing key semantics
        stored.response_id = incomingResponseId;
    }

    stored.last_active_time = time(nullptr);

    // Return the merged session state to caller
    session = stored;
    if (!incomingResponseId.empty()) {
        session.response_id = incomingResponseId;
    }
}

void chatSession::initializeNewSession(const std::string& sessionId, session_st& session)
{
    session.preConversationId = sessionId;
    session.curConversationId = sessionId;
    session.apiChatinfoConversationId = sessionId;
    if (session.created_time == 0) {
        session.created_time = time(nullptr);
    }
    if (session.last_active_time == 0) {
        session.last_active_time = time(nullptr);
    }
    addSession(sessionId, session);
}

// ========== 外层包装方法（含模式判断）==========

session_st& chatSession::createOrUpdateChatSession(session_st& session)
{
    LOG_INFO << "[Chat API] 创建或更新会话";
    
    // 根据配置选择会话追踪模式
    if (isZeroWidthMode()) {
        LOG_INFO << "[Chat API] 使用零宽字符追踪模式";
        return createOrUpdateSessionZeroWidth(session);
    } else {
        LOG_INFO << "[Chat API] 使用Hash追踪模式";
        return createOrUpdateSessionHash(session);
    }
}

session_st& chatSession::createOrUpdateResponseSession(session_st& session)
{
    LOG_INFO << "[Response API] 创建或更新会话";
    
    // 优先级1：如果客户端携带了 previous_response_id，从历史会话获取上下文
    if (session.has_previous_response_id && !session.curConversationId.empty()) {
        LOG_INFO << "[Response API] 检测到 previous_response_id: " << session.curConversationId;
        
        // 尝试从历史会话获取上下文
        session_st prevSession;
        if (getResponseSession(session.curConversationId, prevSession)) {
            // 继承历史消息上下文
            session.message_context = prevSession.message_context;
            session.preConversationId = session.curConversationId;
            LOG_INFO << "[Response API] 从 previous_response_id 继承上下文, 消息数: "
                     << session.message_context.size();
        } else {
            LOG_WARN << "[Response API] previous_response_id 对应的会话不存在: "
                     << session.curConversationId;
        }
        
        return createOrUpdateSessionByPreviousResponseId(session);
    }
    
    // 优先级2：没有 previous_response_id，根据配置选择追踪模式（与Chat API相同逻辑）
    if (isZeroWidthMode()) {
        LOG_INFO << "[Response API] 使用零宽字符追踪模式";
        return createOrUpdateSessionZeroWidth(session);
    } else {
        LOG_INFO << "[Response API] 使用Hash追踪模式";
        return createOrUpdateSessionHash(session);
    }
}

// ========== 底层实现方法（独立功能）==========

session_st& chatSession::createOrUpdateSessionByPreviousResponseId(session_st& session)
{
    LOG_INFO << "[Previous Response ID] 基于previous_response_id创建或更新会话";
    
    std::string prevId = session.curConversationId;
    
    if (!prevId.empty() && sessionIsExist(session)) {
        // 会话存在，更新并延续上下文
        LOG_INFO << "[Previous Response ID] 延续已存在会话: " << prevId;
        updateExistingSessionFromRequest(prevId, session);
    } else {
        // 会话不存在，创建新会话（使用previous_response_id作为会话ID）
        LOG_INFO << "[Previous Response ID] 会话不存在，创建新会话: " << prevId;
        if (!prevId.empty()) {
            initializeNewSession(prevId, session);
        } else {
            // 如果prevId为空，生成新的会话ID
            std::string newSessionId = generateResponseId();
            initializeNewSession(newSessionId, session);
        }
    }
    
    return session;
}

session_st& chatSession::createOrUpdateSessionHash(session_st& session)
{
    LOG_INFO << "[Hash模式] 基于消息内容哈希创建或更新会话";
    
    // Hash 模式：基于消息内容计算会话ID
    std::string tempConversationId = generateConversationKey(generateJsonbySession(session, false));
    LOG_DEBUG << "[Hash模式] 从请求生成会话ID: " << tempConversationId;

    if (sessionIsExist(tempConversationId))
    {
        LOG_DEBUG << "[Hash模式] 会话已存在, 正在更新";
        updateExistingSessionFromRequest(tempConversationId, session);
    }
    else
    {
        if (context_map.find(tempConversationId) != context_map.end())
        {
            LOG_DEBUG << "[Hash模式] 在上下文映射中找到会话";
            std::string mappedSessionId = context_map[tempConversationId];
            session_map[mappedSessionId].contextIsFull = true;
            updateExistingSessionFromRequest(mappedSessionId, session);
            context_map.erase(tempConversationId);
        }
        else
        {
            LOG_INFO << "[Hash模式] 未找到会话, 正在创建新会话";
            initializeNewSession(tempConversationId, session);
        }
    }
    
    return session;
}

// ========== 向后兼容的废弃方法 ==========

session_st& chatSession::createNewSessionOrUpdateSession(session_st& session)
{
    // 废弃方法：重定向到新的外层包装方法
    // 保持原有行为：优先检查 previous_response_id，然后根据模式选择
    LOG_WARN << "[DEPRECATED] createNewSessionOrUpdateSession() 已废弃，请使用 createOrUpdateChatSession() 或 createOrUpdateResponseSession()";
    
    // 如果有 previous_response_id，使用 Response API 的逻辑
    if (session.has_previous_response_id) {
        return createOrUpdateResponseSession(session);
    }
    
    // 否则使用 Chat API 的逻辑
    return createOrUpdateChatSession(session);
}

std::string chatSession::generateConversationKey(
        const Json::Value& keyData
    )
{   // 生成哈希
    // 使用 StyledWriter 保证 JSON 键的顺序，从而确保哈希值的一致性。
    // FastWriter 或默认的 StreamWriterBuilder 不保证顺序。
    Json::StyledWriter writer;
    std::string output = writer.write(keyData);
    return generateSHA256(output);
}

void chatSession::coverSessionresponse(session_st& session)
{
    Json::Value assistantresponse;
    assistantresponse["role"]="user";
    assistantresponse["content"]=session.requestmessage_raw.empty() ? session.requestmessage : session.requestmessage_raw;
    session.addMessageToContext(assistantresponse);
    assistantresponse["role"]="assistant";
    assistantresponse["content"]=session.responsemessage["message"].asString();
    session.addMessageToContext(assistantresponse);
    session.last_active_time=time(nullptr);

    // These are per-request transient fields. Persisting them in session_map is wasteful,
    // especially for tool-calling clients where the "current input" can be huge.
    session.requestmessage.clear();
    session.requestmessage_raw.clear();
    session.responsemessage.clear();
    session.tool_bridge_trigger.clear();
    
    // 零宽字符模式：会话ID保持不变，只更新session内容
    if (isZeroWidthMode()) {
        LOG_INFO << "[ZeroWidth] 更新会话内容，会话ID保持不变: " << session.curConversationId;
        updateSession(session.curConversationId, session);
        return;
    }
    
    // Hash模式：原有逻辑，根据内容生成新的会话ID
    std::string newConversationId;
    newConversationId=chatSession::getInstance()->generateConversationKey(
    chatSession::getInstance()->generateJsonbySession(session,session.contextIsFull)
    );
    session.preConversationId=session.curConversationId;
    session.curConversationId = newConversationId;
    if (!session.selectapi.empty()) {
        auto api = ApiManager::getInstance().getApiByApiName(session.selectapi);
        if (api) {
            api->transferThreadContext(session.preConversationId, session.curConversationId);
        }
    }
    addSession(newConversationId,session);
    delSession(session.preConversationId);
    if(!session.contextIsFull)
    {
        LOG_INFO << "上下文未满，更新上下文长度,生成新的contextConversationId";
        session.contextlength=session.message_context.size()-2;
        std::string tempConversationId=generateConversationKey(generateJsonbySession(session,true));
        context_map.erase(session.contextConversationId);
        session.contextConversationId=tempConversationId;
        context_map[tempConversationId]=session.curConversationId;
        updateSession(session.curConversationId,session);
    }
}
std::string chatSession::generateSHA256(const std::string& input) {
    //计算sha256的耗时
    auto start_time = std::chrono::high_resolution_clock::now();
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    // 使用新的 EVP 接口
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hashLen;
        // 创建 EVP_MD_CTX 上下文
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) {
            throw std::runtime_error("Failed to create EVP_MD_CTX");
        }

        try {
            // 初始化上下文
            if (1 != EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr)) {
                throw std::runtime_error("Failed to initialize digest");
            }

            // 更新数据
            if (1 != EVP_DigestUpdate(ctx, input.c_str(), input.length())) {
                throw std::runtime_error("Failed to update digest");
            }

            // 完成哈希计算
            if (1 != EVP_DigestFinal_ex(ctx, hash, &hashLen)) {
                throw std::runtime_error("Failed to finalize digest");
            }

            // 转换为十六进制字符串
            std::stringstream ss;
            for (unsigned int i = 0; i < hashLen; i++) {
                ss << std::hex << std::setw(2) << std::setfill('0') 
                   << static_cast<int>(hash[i]);
            }

            // 清理上下文
            EVP_MD_CTX_free(ctx);
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
            LOG_INFO << "sha256计算耗时: " << duration.count() << " 微秒";
            return ss.str();
        }
        catch (...) {
            // 确保在发生异常时也能清理上下文
            EVP_MD_CTX_free(ctx);
            throw;
        }
#else
    // 使用旧的接口
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, input.c_str(), input.length());
        SHA256_Final(hash, &sha256);

        std::stringstream ss;
        for(int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        LOG_INFO << "sha256计算耗时: " << duration.count() << " 微秒";  
        return ss.str();
#endif
 }
Json::Value chatSession::getClientInfo(const HttpRequestPtr &req)
{
    Json::Value clientInfo;

        // 获取客户端IP
        //clientInfo["ip"] = req->getPeerAddr().toIp();
        
        // 获取User-Agent
        // auto userAgent = req->getHeader("User-Agent");
        // if (!userAgent.empty()) {
        //     clientInfo["user_agent"] = userAgent;
        // }
        /*
        // 获取其他相关头部信息
        std::vector<std::string> importantHeaders = {
            "X-Forwarded-For",
            "X-Real-IP"
        };

        for (const auto& header : importantHeaders) {
            auto value = req->getHeader(header);
            if (!value.empty()) {
                clientInfo["headers"][header] = value;
            }
        }
        */
        std::string userAgent = req->getHeader("user-agent");
        std::string clientType = ""; // 默认为空字符串

        if (userAgent.find("Kilo-Code") != std::string::npos) {
            clientType = "Kilo-Code";
        } 
        else if (userAgent.find("RooCode") != std::string::npos) {
            clientType = "RooCode"; // RooCode 和 Kilo 逻辑基本一致
        }
        // else if (userAgent.find("Continue") ...) { clientType = "Continue"; } // 以后可以这样扩展

        // 将字符串标识存入 session
        clientInfo["client_type"] = clientType; 

        std::string auth = req->getHeader("authorization");
        if (auth.empty()) auth = req->getHeader("Authorization");
        auto stripBearer = [](std::string &s) {
            const std::string p1 = "Bearer ";
            const std::string p2 = "bearer ";
            if (s.rfind(p1, 0) == 0) s = s.substr(p1.size());
            else if (s.rfind(p2, 0) == 0) s = s.substr(p2.size());
            // 简单 trim
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        };
        stripBearer(auth);
        clientInfo["client_authorization"]=auth;
        LOG_INFO << "识别到客户端类型: " << (clientType.empty() ? "Unknown" : clientType);
        LOG_INFO << "识别到客户 authorization: " << (auth.empty() ? "empty" : auth);
        return clientInfo;
}
// void chatSession::clearExpiredSession()
// {
//     LOG_INFO << "开始清除过期会话，当前会话数量:" << session_map.size();
//     time_t now = time(nullptr);
//     //遍历session_map,删除过期会话
//     for(auto it = session_map.begin(); it != session_map.end();)
//     {
//         if (now - it->second.last_active_time > SESSION_EXPIRE_TIME) {
//             {
//                 std::lock_guard<std::mutex> lock(mutex_);
//                 it = session_map.erase(it);
//             }
//             if(it->second.selectapi=="chaynsapi")
//             {
//                 std::lock_guard<std::mutex> lock(mutex_);
//                 ApiManager::getInstance().getApiByApiName(it->second.selectapi)->eraseChatinfoMap(it->first);
//             }
//         } else {
//             ++it;
//         }
//     }
//     LOG_INFO << "清除过期会话完成，剩余会话数量:" << session_map.size();
// }

void chatSession::clearExpiredSession()
{
    // 存储过期会话信息: (sessionId, selectapi, is_response_api)
    std::vector<std::tuple<std::string, std::string, bool>> expired;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        int chatCount = 0, responseCount = 0;
        for (const auto& pair : session_map) {
            if (pair.second.is_response_api) responseCount++;
            else chatCount++;
        }
        LOG_INFO << "开始清除过期会话，当前会话数量:" << session_map.size()
                 << " (Chat: " << chatCount << ", Response: " << responseCount << ")";
        time_t now = time(nullptr);

        for (auto it = session_map.begin(); it != session_map.end();)
        {
            if (now - it->second.last_active_time > SESSION_EXPIRE_TIME)
            {
                const std::string sessionId = it->first;
                const std::string apiName = it->second.selectapi;
                const bool isResponseApi = it->second.is_response_api;
                expired.emplace_back(sessionId, apiName, isResponseApi);

                it = session_map.erase(it);

                // Chat API 会话：清理 context_map
                // Response API 会话：不使用 context_map，无需清理
                if (!isResponseApi) {
                    for (auto ctxIt = context_map.begin(); ctxIt != context_map.end();)
                    {
                        if (ctxIt->second == sessionId)
                            ctxIt = context_map.erase(ctxIt);
                        else
                            ++ctxIt;
                    }
                }
            }
            else
            {
                ++it;
            }
        }

        chatCount = 0; responseCount = 0;
        for (const auto& pair : session_map) {
            if (pair.second.is_response_api) responseCount++;
            else chatCount++;
        }
        LOG_INFO << "清除过期会话完成，剩余会话数量:" << session_map.size()
                 << " (Chat: " << chatCount << ", Response: " << responseCount << ")";
    } // 解锁后再清 provider，避免锁顺序/死锁风险

    // 清理 API provider 资源
    for (const auto& item : expired)
    {
        const std::string& sessionId = std::get<0>(item);
        const std::string& apiName = std::get<1>(item);
        const bool isResponseApi = std::get<2>(item);
        
        if (apiName.empty())
            continue;

        auto api = ApiManager::getInstance().getApiByApiName(apiName);
        if (api)
        {
            api->eraseChatinfoMap(sessionId);
        }
        
        if (isResponseApi) {
            LOG_DEBUG << "[Response API] 清理过期会话: " << sessionId;
        }
    }
}
void chatSession::startClearExpiredSession()
{
    std::thread([this]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(SESSION_EXPIRE_TIME));
            clearExpiredSession();
        }
    }).detach();
}   

bool chatSession::sessionIsExist(const std::string &ConversationId)
{
    //查到并且model也一样
    auto it=session_map.find(ConversationId);
    if(it!=session_map.end())
    {
            return true;
    }
    return false;
}
bool chatSession::sessionIsExist(session_st &session)
{
    //查到并且model也一样
    auto it=session_map.find(session.curConversationId);
    if(it!=session_map.end())
    {
        if(it->second.selectmodel==session.selectmodel&&it->second.selectapi==session.selectapi)
        {
            return true;
        }
    }
    return false;

}

// ========== 图片解析辅助方法实现 (Chat API 和 Response API 共用) ==========

void chatSession::extractImagesFromContent(const Json::Value& content, std::vector<ImageInfo>& images)
{
    if (!content.isArray()) return;
    for (const auto& item : content) {
        parseImageItem(item, images);
    }
}

void chatSession::parseImageItem(const Json::Value& item, std::vector<ImageInfo>& images)
{
    if (!item.isObject() || !item.isMember("type") || item["type"].asString() != "image_url") {
        return;
    }
    if (!item.isMember("image_url") || !item["image_url"].isObject()) {
        return;
    }
    
    const auto& imageUrl = item["image_url"];
    if (!imageUrl.isMember("url") || !imageUrl["url"].isString()) {
        return;
    }
    
    std::string url = imageUrl["url"].asString();
    ImageInfo imgInfo;
    
    // 检查是否是 base64 编码的图片
    if (url.find("data:") == 0) {
        // 格式: data:image/png;base64,xxxxx
        size_t semicolonPos = url.find(";");
        size_t commaPos = url.find(",");
        if (semicolonPos != std::string::npos && commaPos != std::string::npos) {
            imgInfo.mediaType = url.substr(5, semicolonPos - 5); // 提取 image/png
            imgInfo.base64Data = url.substr(commaPos + 1);       // 提取 base64 数据
        }
    } else {
        // 直接是URL
        imgInfo.uploadedUrl = url;
    }
    
    if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
        images.push_back(imgInfo);
    }
}

std::string chatSession::getContentAsString(const Json::Value& content, std::vector<ImageInfo>& images)
{
    if (content.isString()) {
        std::string text = content.asString();
        // 如果使用零宽字符模式，从文本中移除可能存在的零宽字符编码
        if (getInstance()->isZeroWidthMode()) {
            ZeroWidthEncoder::stripZeroWidth(text);
        }
        return text;
    }
    if (content.isArray()) {
        std::string result;
        for (const auto& item : content) {
            if (item.isObject() && item.isMember("type")) {
                std::string itemType = item["type"].asString();
                // 处理文本类型：支持 "text" (Chat API) 和 "input_text" (Response API)
                if ((itemType == "text" || itemType == "input_text") && item.isMember("text") && item["text"].isString()) {
                    std::string textPart = item["text"].asString();
                    // 如果使用零宽字符模式，从文本中移除可能存在的零宽字符编码
                    if (getInstance()->isZeroWidthMode()) {
                        ZeroWidthEncoder::stripZeroWidth(textPart);
                    }
                    result += textPart;
            
                    // 如果当前片段不以换行符结尾，手动补一个换行
                    if (!textPart.empty() && textPart.back() != '\n') {
                        result += "\n";
                    }
                }
                // 处理图片类型：支持 "image_url" (Chat API)
                else if (itemType == "image_url") {
                    parseImageItem(item, images);
                }
                // 处理图片类型：支持 "input_image" (Response API)
                else if (itemType == "input_image") {
                    ImageInfo imgInfo;
                    // Response API 的 input_image 格式可能有多种
                    std::string url;
                    if (item.isMember("image_url") && item["image_url"].isString()) {
                        url = item["image_url"].asString();
                    } else if (item.isMember("url") && item["url"].isString()) {
                        url = item["url"].asString();
                    } else if (item.isMember("file") && item["file"].isObject()) {
                        const auto& fileObj = item["file"];
                        if (fileObj.isMember("url") && fileObj["url"].isString()) {
                            url = fileObj["url"].asString();
                        }
                    }
                    
                    if (!url.empty()) {
                        if (url.find("data:") == 0) {
                            size_t semicolon = url.find(';');
                            size_t comma = url.find(',');
                            if (semicolon != std::string::npos && comma != std::string::npos) {
                                imgInfo.mediaType = url.substr(5, semicolon - 5);
                                imgInfo.base64Data = url.substr(comma + 1);
                            }
                        } else {
                            imgInfo.uploadedUrl = url;
                        }
                        
                        if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                            images.push_back(imgInfo);
                            LOG_DEBUG << "[getContentAsString] 提取到图片(input_image), mediaType: " << imgInfo.mediaType
                                     << ", hasBase64: " << (!imgInfo.base64Data.empty())
                                     << ", hasUrl: " << (!imgInfo.uploadedUrl.empty());
                        }
                    }
                }
            } else if (item.isObject() && item.isMember("text") && item["text"].isString()) {
                // 兼容旧格式
                std::string textPart = item["text"].asString();
                // 如果使用零宽字符模式，从文本中移除可能存在的零宽字符编码
                if (getInstance()->isZeroWidthMode()) {
                    ZeroWidthEncoder::stripZeroWidth(textPart);
                }
                result += textPart;
                if (!textPart.empty() && textPart.back() != '\n') {
                    result += "\n";
                }
            }
        }
        return result;
    }
    return "";
}

// ========== 会话生成方法 ==========

session_st chatSession::gennerateSessionstByReq(const HttpRequestPtr &req)
{
    LOG_INFO<<__FILE__<<":"<<__FUNCTION__<<":"<<__LINE__<<"开始生成session_st";
    session_st session;
    Json::Value requestbody=*req->getJsonObject();
    session.client_info = getClientInfo(req);
    session.selectmodel = requestbody["model"].asString();
    
    int splitIndex=-1;
    for(int i = requestbody["messages"].size()-1; i > 0; i--)
    {
        if(requestbody["messages"][i]["role"]=="assistant")
        {
            splitIndex=i;
            break;
        }
    }
    
    // 零宽字符模式：从最后一条 assistant 消息中提取嵌入的会话ID
    std::string extractedZeroWidthSessionId;
    if (getInstance()->isZeroWidthMode() && splitIndex > 0) {
        const Json::Value& lastAssistantContent = requestbody["messages"][splitIndex]["content"];
        std::string lastAssistantText;
        if (lastAssistantContent.isString()) {
            lastAssistantText = lastAssistantContent.asString();
        } else if (lastAssistantContent.isArray()) {
            for (const auto& item : lastAssistantContent) {
                if (item.isObject() && item.isMember("type") && item["type"].asString() == "text") {
                    lastAssistantText += item.get("text", "").asString();
                }
            }
        }
        // 提取会话ID（不修改原文本，只读取）
        extractedZeroWidthSessionId = extractSessionIdFromText(lastAssistantText);
        if (!extractedZeroWidthSessionId.empty()) {
            LOG_INFO << "[ZeroWidth] 从历史assistant消息中提取到会话ID: " << extractedZeroWidthSessionId;
            session.curConversationId = extractedZeroWidthSessionId;
            session.preConversationId = extractedZeroWidthSessionId;
        }
    }
    
    // 用于临时存储历史消息中的图片（这些图片我们暂不处理，只存储当前请求的图片）
    std::vector<ImageInfo> tempImages;
    
    for(int i = 0; i <requestbody["messages"].size(); i++)
    {
        if(requestbody["messages"][i]["role"] == "system")
            {
                session.systemprompt = session.systemprompt + getContentAsString(requestbody["messages"][i]["content"], tempImages);
                continue;
            }
        if(i<=splitIndex)
        {
            // 合并历史记录中的连续 user 消息
            if (requestbody["messages"][i]["role"] == "user" &&
                !session.message_context.empty() &&
                session.message_context[session.message_context.size() - 1]["role"] == "user")
            {
                session.message_context[session.message_context.size() - 1]["content"] =
                    session.message_context[session.message_context.size() - 1]["content"].asString() +
                    getContentAsString(requestbody["messages"][i]["content"], tempImages);
            } else {
                Json::Value msgData;
                msgData["role"] = requestbody["messages"][i]["role"];
                msgData["content"] = getContentAsString(requestbody["messages"][i]["content"], tempImages);
                session.addMessageToContext(msgData);
            }
        }
        else
        {
            if(requestbody["messages"][i]["role"]=="user")
            {
                // 提取当前用户请求的文本和图片
                std::string user_content = getContentAsString(requestbody["messages"][i]["content"], session.requestImages);
                if (session.requestmessage.empty()) {
                    session.requestmessage = user_content;
                } else {
                    session.requestmessage += user_content;
                }
            }
        }
    }
    
       
    session.created_time = time(NULL);     // 统一设置创建时间
    session.last_active_time = time(NULL);
    LOG_INFO<<__FILE__<<":"<<__FUNCTION__<<":"<<__LINE__<<"生成session_st完成";
    LOG_INFO << "[会话管理] session_st消息上下文: " << Json::FastWriter().write(session.message_context);
    return session;
}
Json::Value chatSession::generateJsonbySession(const session_st& session,bool contextIsFull)
{
     Json::Value keyData;
     Json::Value messages(Json::arrayValue);
     int startIndex=contextIsFull?(session.message_context.size()-session.contextlength):0;
     for(int i=startIndex;i<session.message_context.size();i++)
     {
        messages.append(session.message_context[i]);
     }
     keyData["messages"] = messages;
     keyData["client_info"] = session.client_info;
     keyData["model"] = session.selectmodel;
     Json::StreamWriterBuilder writer;
     writer["emitUTF8"] = true;  // 确保输出UTF-8编码
     LOG_INFO << "生成ConversationId使用的数据: " << Json::writeString(writer,keyData);
     return keyData;
}

// ========== Response API 方法实现 ==========

std::string chatSession::generateResponseId()
{
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return "resp_" + std::to_string(timestamp) + "_" + std::to_string(dis(gen));
}

std::string chatSession::createResponseSession(session_st& session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 如果调用者已经设置了 response_id，则使用它；否则生成新的
    std::string respId = session.response_id.empty() ? generateResponseId() : session.response_id;
    session.response_id = respId;
    session.curConversationId = respId;  // 直接使用 response_id
    session.is_response_api = true;
    if (session.created_time == 0) {
        session.created_time = time(nullptr);
    }
    if (session.last_active_time == 0) {
        session.last_active_time = time(nullptr);
    }
    
    // 直接用 response_id 作为 session_map 的键
    session_map[respId] = session;
    
    LOG_INFO << "[Response API] 创建会话, response_id: " << respId;
    return respId;
}

bool chatSession::getResponseSession(const std::string& response_id, session_st& session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 直接查找，不需要映射
    auto it = session_map.find(response_id);
    if (it == session_map.end()) {
        LOG_WARN << "[Response API] getResponseSession: response_id not found: " << response_id;
        return false;
    }
    
    session = it->second;
    return true;
}

bool chatSession::deleteResponseSession(const std::string& response_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = session_map.find(response_id);
    if (it == session_map.end()) {
        LOG_WARN << "[Response API] deleteResponseSession: response_id not found: " << response_id;
        return false;
    }
    
    // 如果有关联的 API，清理其资源
    const std::string& apiName = it->second.selectapi;
    if (!apiName.empty()) {
        auto api = ApiManager::getInstance().getApiByApiName(apiName);
        if (api) {
            api->eraseChatinfoMap(response_id);
        }
    }
    
    session_map.erase(it);
    LOG_INFO << "[Response API] 删除会话, response_id: " << response_id;
    return true;
}

void chatSession::updateResponseSession(session_st& session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (session.response_id.empty()) {
        LOG_WARN << "[Response API] updateResponseSession: response_id is empty";
        return;
    }
    
    auto it = session_map.find(session.response_id);
    if (it == session_map.end()) {
        LOG_WARN << "[Response API] updateResponseSession: response_id not found: " << session.response_id;
        return;
    }
    
    // Response API 不删除旧 session，直接更新
    // response_id 保持不变
    session.last_active_time = time(nullptr);
    
    // 更新上下文
    Json::Value userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = session.requestmessage_raw.empty() ? session.requestmessage : session.requestmessage_raw;
    session.addMessageToContext(userMsg);
    
    Json::Value assistantMsg;
    assistantMsg["role"] = "assistant";
    assistantMsg["content"] = session.responsemessage["message"].asString();
    session.addMessageToContext(assistantMsg);

    // 清理临时数据（避免把超大输入/输出持久化到 session_map）
    session.requestmessage.clear();
    session.requestmessage_raw.clear();
    session.responsemessage.clear();
    session.tool_bridge_trigger.clear();

    // 直接更新，不改变键
    session_map[session.response_id] = session;
    
    LOG_INFO << "[Response API] 更新会话, response_id: " << session.response_id;
}

bool chatSession::updateResponseApiData(const std::string& response_id, const Json::Value& apiData)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (response_id.empty()) {
        LOG_WARN << "[Response API] updateResponseApiData: response_id is empty";
        return false;
    }

    auto it = session_map.find(response_id);
    if (it == session_map.end()) {
        LOG_WARN << "[Response API] updateResponseApiData: response_id not found: " << response_id
                 << ", creating minimal session";

        session_st s;
        s.response_id = response_id;
        s.curConversationId = response_id;
        s.is_response_api = true;
        s.created_time = time(nullptr);
        s.last_active_time = time(nullptr);
        s.api_response_data = apiData;
        session_map[response_id] = std::move(s);
        return false;
    }

    it->second.api_response_data = apiData;
    it->second.last_active_time = time(nullptr);
    return true;
}

session_st chatSession::gennerateSessionstByResponseReq(const HttpRequestPtr &req)
{
    LOG_INFO << __FILE__ << ":" << __FUNCTION__ << ":" << __LINE__ << "开始生成 Response API session_st";
    session_st session;
    
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        LOG_WARN << "[Response API] 无效的 JSON 请求体";
        return session;
    }
    
    const Json::Value& reqBody = *jsonPtr;
    
    // 1. 初始化基本参数
    session.selectmodel = reqBody.get("model", "GPT-4o").asString();
    session.selectapi = "chaynsapi";
    session.systemprompt = reqBody.get("instructions", "").asString();
    session.client_info = getClientInfo(req);
    session.is_response_api = true;
    session.last_active_time = time(nullptr);
    session.created_time = time(nullptr);
    
    // 2. 处理 input 字段
    if (reqBody.isMember("input")) {
        const auto& input = reqBody["input"];
        
        if (input.isString()) {
            // 简单字符串输入
            session.requestmessage = input.asString();
        } else if (input.isArray()) {
            // 数组格式：需要分离历史消息和当前请求
            int splitIndex = findLastAssistantIndexInInput(input);
            LOG_INFO << "[Response API] splitIndex: " << splitIndex << ", input size: " << input.size();
            
            bool hasPreviousResponseId = reqBody.isMember("previous_response_id") && 
                                         !reqBody["previous_response_id"].asString().empty();
            bool isZeroWidthMode = getInstance()->isZeroWidthMode();
            
            // 零宽字符模式：从历史 assistant 消息中提取会话ID
            if (!hasPreviousResponseId && isZeroWidthMode && splitIndex >= 0) {
                std::string extractedSessionId = extractZeroWidthSessionIdFromResponseInput(input, splitIndex);
                if (!extractedSessionId.empty()) {
                    LOG_INFO << "[Response API][ZeroWidth] 从历史assistant消息中提取到会话ID: " << extractedSessionId;
                    session.curConversationId = extractedSessionId;
                    session.preConversationId = extractedSessionId;
                }
            }
            
            // 解析 input 数组中的每个元素
            for (int i = 0; i < static_cast<int>(input.size()); i++) {
                parseResponseInputItem(input[i], i, splitIndex, session, isZeroWidthMode);
            }
        }
    }
    
    // 3. 处理 previous_response_id（优先级最高，会覆盖其他会话追踪方式）
    if (reqBody.isMember("previous_response_id") && !reqBody["previous_response_id"].asString().empty()) {
        handlePreviousResponseId(reqBody["previous_response_id"].asString(), session);
    }
    
    LOG_INFO << "[Response API] message_context size: " << session.message_context.size()
             << ", requestmessage length: " << session.requestmessage.length();
    LOG_INFO << __FILE__ << ":" << __FUNCTION__ << ":" << __LINE__ << "生成 Response API session_st 完成";
    return session;
}

// ========== 零宽字符追踪模式方法实现 ==========

std::string chatSession::generateZeroWidthSessionId()
{
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return "zw_" + std::to_string(timestamp) + "_" + std::to_string(dis(gen));
}

session_st& chatSession::createOrUpdateSessionZeroWidth(session_st& session)
{
    LOG_INFO << "[ZeroWidth] 开始处理零宽字符模式会话";
    
    // 会话ID已经在 gennerateSessionstByReq 中从历史assistant消息提取
    // 这里检查 curConversationId 是否已设置
    std::string extractedSessionId = session.curConversationId;
    
    if (!extractedSessionId.empty() && sessionIsExist(session))
    {
        // 找到了有效的会话ID，更新现有会话
        LOG_INFO << "[ZeroWidth] 使用已提取的会话ID: " << extractedSessionId;
        updateExistingSessionFromRequest(extractedSessionId, session);
    }
    else
    {
        // 没有找到有效的会话ID，创建新会话
        LOG_INFO << "[ZeroWidth] 未找到有效会话ID，创建新会话";
        std::string newSessionId = generateZeroWidthSessionId();
        initializeNewSession(newSessionId, session);
    }
    
    return session;
}

std::string chatSession::extractSessionIdFromText(const std::string& text)
{
    auto result = ZeroWidthEncoder::decode(text);
    return result.value_or("");
}

std::string chatSession::extractAndRemoveSessionIdFromText(std::string& text)
{
    auto result = ZeroWidthEncoder::extractAndRemove(text);
    return result.value_or("");
}

std::string chatSession::embedSessionIdInText(const std::string& text, const std::string& sessionId)
{
    if (sessionId.empty()) {
        return text;
    }
    return ZeroWidthEncoder::appendEncoded(text, sessionId);
}

// ========== Response API 专用辅助方法实现 ==========

int chatSession::findLastAssistantIndexInInput(const Json::Value& input)
{
    if (!input.isArray()) return -1;
    
    for (int i = input.size() - 1; i >= 0; i--) {
        const auto& item = input[i];
        if (item.isObject()) {
            std::string role = item.get("role", "").asString();
            if (role == "assistant") {
                return i;
            }
        }
    }
    return -1;
}

std::string chatSession::extractTextFromResponseContent(const Json::Value& content, bool stripZeroWidth)
{
    std::string result;
    
    if (content.isString()) {
        result = content.asString();
    } else if (content.isArray()) {
        for (const auto& c : content) {
            if (c.isObject()) {
                std::string ctype = c.get("type", "").asString();
                if (ctype == "output_text" || ctype == "text" || ctype == "input_text") {
                    result += c.get("text", "").asString();
                }
            }
        }
    }
    
    if (stripZeroWidth && !result.empty()) {
        ZeroWidthEncoder::stripZeroWidth(result);
    }
    
    return result;
}

std::string chatSession::extractZeroWidthSessionIdFromResponseInput(const Json::Value& input, int splitIndex)
{
    if (splitIndex < 0 || !input.isArray() || splitIndex >= static_cast<int>(input.size())) {
        return "";
    }
    
    const auto& lastAssistantItem = input[splitIndex];
    std::string lastAssistantText;
    
    if (lastAssistantItem.isMember("content")) {
        lastAssistantText = extractTextFromResponseContent(lastAssistantItem["content"], false);
    }
    
    return extractSessionIdFromText(lastAssistantText);
}

void chatSession::parseResponseInputItem(const Json::Value& item, int index, int splitIndex,
                                         session_st& session, bool isZeroWidthMode)
{
    if (item.isString()) {
        // 简单字符串，作为当前请求的一部分
        session.requestmessage += item.asString() + "\n";
        return;
    }
    
    if (!item.isObject()) return;
    
    std::string type = item.get("type", "").asString();
    std::string role = item.get("role", "").asString();
    
    // 处理带 role 的消息（历史对话格式）
    if (!role.empty()) {
        if (index <= splitIndex) {
            // 历史消息：添加到 message_context（使用临时图片存储，不加入当前请求图片）
            std::vector<ImageInfo> historyImages;
            std::string msgContent;
            
            if (item.isMember("content")) {
                const auto& content = item["content"];
                if (content.isString()) {
                    msgContent = content.asString();
                    if (isZeroWidthMode) {
                        ZeroWidthEncoder::stripZeroWidth(msgContent);
                    }
                } else if (content.isArray()) {
                    msgContent = getContentAsString(content, historyImages);
                }
            }
            
            Json::Value msgData;
            msgData["role"] = role;
            msgData["content"] = msgContent;
            session.addMessageToContext(msgData);
        } else {
            // 当前请求：只处理 user 消息
            if (role == "user") {
                std::string msgContent;
                if (item.isMember("content")) {
                    const auto& content = item["content"];
                    if (content.isString()) {
                        msgContent = content.asString();
                        if (isZeroWidthMode) {
                            ZeroWidthEncoder::stripZeroWidth(msgContent);
                        }
                    } else if (content.isArray()) {
                        msgContent = getContentAsString(content, session.requestImages);
                    }
                }
                session.requestmessage += msgContent;
            }
        }
        return;
    }
    
    // 处理简单的 input_text 类型（没有 role）
    if (type == "input_text" || type == "text") {
        std::string textContent = item.get("text", "").asString();
        if (isZeroWidthMode) {
            ZeroWidthEncoder::stripZeroWidth(textContent);
        }
        session.requestmessage += textContent;
        return;
    }
    
    // 处理图片输入 (input_image 类型)
    if (type == "input_image") {
        ImageInfo imgInfo;
        std::string url;
        
        if (item.isMember("image_url")) {
            url = item["image_url"].asString();
        } else if (item.isMember("url")) {
            url = item["url"].asString();
        } else if (item.isMember("file") && item["file"].isObject()) {
            const auto& fileObj = item["file"];
            if (fileObj.isMember("url")) {
                url = fileObj["url"].asString();
            }
        }
        
        if (!url.empty()) {
            if (url.find("data:") == 0) {
                size_t semicolon = url.find(';');
                size_t comma = url.find(',');
                if (semicolon != std::string::npos && comma != std::string::npos) {
                    imgInfo.mediaType = url.substr(5, semicolon - 5);
                    imgInfo.base64Data = url.substr(comma + 1);
                }
            } else {
                imgInfo.uploadedUrl = url;
            }
            
            if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                session.requestImages.push_back(imgInfo);
                LOG_INFO << "[Response API] 提取到图片, mediaType: " << imgInfo.mediaType
                         << ", hasBase64: " << (!imgInfo.base64Data.empty())
                         << ", hasUrl: " << (!imgInfo.uploadedUrl.empty());
            }
        }
        return;
    }
    
    // 处理 image_url 类型（兼容 Chat API 格式）
    if (type == "image_url") {
        std::vector<ImageInfo> singleImage;
        parseImageItem(item, singleImage);
        for (const auto& img : singleImage) {
            session.requestImages.push_back(img);
            LOG_INFO << "[Response API] 提取到图片(image_url格式), mediaType: " << img.mediaType
                     << ", hasBase64: " << (!img.base64Data.empty())
                     << ", hasUrl: " << (!img.uploadedUrl.empty());
        }
    }
}

void chatSession::handlePreviousResponseId(const std::string& prevId, session_st& session)
{
    session.has_previous_response_id = true;
    session_st prevSession;
    
    if (getResponseSession(prevId, prevSession)) {
        // 从之前的响应中提取上下文
        if (prevSession.api_response_data.isMember("_internal_session_id")) {
            std::string internalSessionId = prevSession.api_response_data["_internal_session_id"].asString();
            session.curConversationId = internalSessionId;
            session.preConversationId = internalSessionId;
            LOG_INFO << "[Response API] 使用 previous_response_id 延续会话: " << internalSessionId;
        }
        // 继承之前的消息上下文（覆盖 input 中可能存在的历史消息）
        session.message_context = prevSession.message_context;
    } else {
        LOG_WARN << "[Response API] previous_response_id 不存在: " << prevId;
    }
}
