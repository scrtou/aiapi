#include "Session.h"
#include <time.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include "chaynsapi.h"
#include <apiManager/ApiManager.h>
#include <apiManager/Apicomn.h>
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
        LOG_WARN << "getSession: ConversationId not found";
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


session_st& chatSession::createNewSessionOrUpdateSession(session_st& session)
{

    std::string tempConversationId=generateConversationKey(generateJsonbySession(session,false));
    LOG_INFO << "根据客户端请求生成ConversationId: " << tempConversationId;

    if(sessionIsExist(tempConversationId))
    {
        LOG_INFO<<"会话已存在，更新会话";
        session_map[tempConversationId].requestmessage=session.requestmessage;
        session_map[tempConversationId].last_active_time=session.last_active_time;
        session=session_map[tempConversationId];
    }
    else
    {
        if(context_map.find(tempConversationId) != context_map.end())
        {
            LOG_INFO<<"在上下文会话中存在";
            session_map[context_map[tempConversationId]].requestmessage=session.requestmessage;
            session_map[context_map[tempConversationId]].last_active_time=session.last_active_time;
            session_map[context_map[tempConversationId]].contextIsFull=true;
            session=session_map[context_map[tempConversationId]];
            context_map.erase(tempConversationId);
        }
        else
        {
            LOG_INFO<<"会话不存在，创建会话";
            session.preConversationId=tempConversationId;
            session.curConversationId=tempConversationId;
            session.apiChatinfoConversationId=tempConversationId;
            addSession(tempConversationId,session);
        }
    }
    
    return session;
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
    assistantresponse["content"]=session.requestmessage;
    session.addMessageToContext(assistantresponse);
    assistantresponse["role"]="assistant";
    assistantresponse["content"]=session.responsemessage["message"].asString();
    session.addMessageToContext(assistantresponse);
    session.last_active_time=time(nullptr);
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
    session.requestmessage.clear();
    session.responsemessage.clear();
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
    std::vector<std::pair<std::string, std::string>> expired; // (conversationId, selectapi)

    {
        std::lock_guard<std::mutex> lock(mutex_);
        LOG_INFO << "开始清除过期会话，当前会话数量:" << session_map.size();
        time_t now = time(nullptr);

        for (auto it = session_map.begin(); it != session_map.end();)
        {
            if (now - it->second.last_active_time > SESSION_EXPIRE_TIME)
            {
                const std::string convId = it->first;
                const std::string apiName = it->second.selectapi;
                expired.emplace_back(convId, apiName);

                it = session_map.erase(it);

                // 顺带清理 context_map：删除所有指向该会话的映射
                for (auto ctxIt = context_map.begin(); ctxIt != context_map.end();)
                {
                    if (ctxIt->second == convId)
                        ctxIt = context_map.erase(ctxIt);
                    else
                        ++ctxIt;
                }
            }
            else
            {
                ++it;
            }
        }

        LOG_INFO << "清除过期会话完成，剩余会话数量:" << session_map.size();
    } // 解锁后再清 provider，避免锁顺序/死锁风险

    for (const auto& item : expired)
    {
        const std::string& convId = item.first;
        const std::string& apiName = item.second;
        if (apiName.empty())
            continue;

        auto api = ApiManager::getInstance().getApiByApiName(apiName);
        if (api)
        {
            api->eraseChatinfoMap(convId);
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
    return session_map.find(ConversationId) != session_map.end();
}
session_st chatSession::gennerateSessionstByReq(const HttpRequestPtr &req)
{
    LOG_INFO<<__FILE__<<":"<<__FUNCTION__<<":"<<__LINE__<<"开始生成session_st";
    session_st session;
    Json::Value requestbody=*req->getJsonObject();
    session.client_info = getClientInfo(req);
    session.selectmodel = requestbody["model"].asString();
    


    auto getContentAsString = [](const Json::Value& content) -> std::string {
        if (content.isString()) {
            return content.asString();
        }
        if (content.isArray()) {
            std::string result;
            for (const auto& item : content) {
                if (item.isObject() && item.isMember("text") && item["text"].isString()) {
                    std::string textPart = item["text"].asString();
                    result += textPart;
            
                    // 【可选改进】如果当前片段不以换行符结尾，手动补一个换行
                    // 这样可以确保 <task> 和 <environment_details> 即使在源数据中紧挨着，这里也会分行
                    if (!textPart.empty() && textPart.back() != '\n') {
                        result += "\n";
                    }
                }
            }
            return result;
        }
        return "";
    };
    
    int splitIndex=0;
    for(int i = requestbody["messages"].size()-1; i > 0; i--)
    {
        if(requestbody["messages"][i]["role"]=="assistant")
        {
            splitIndex=i;
            break;
        }
    }
    for(int i = 0; i <requestbody["messages"].size(); i++)
    {
        if(requestbody["messages"][i]["role"] == "system")
            {
                session.systemprompt = session.systemprompt + getContentAsString(requestbody["messages"][i]["content"]);
                continue;
            }
        if(i<=splitIndex)
        {
            // 合并历史记录中的连续 user 消息
            // 合并历史记录中的连续 user 消息
            if (requestbody["messages"][i]["role"] == "user" &&
                !session.message_context.empty() &&
                session.message_context[session.message_context.size() - 1]["role"] == "user")
            {
                session.message_context[session.message_context.size() - 1]["content"] =
                    session.message_context[session.message_context.size() - 1]["content"].asString() +
                    getContentAsString(requestbody["messages"][i]["content"]);
            } else {
                Json::Value msgData;
                msgData["role"] = requestbody["messages"][i]["role"];
                msgData["content"] = getContentAsString(requestbody["messages"][i]["content"]);
                session.addMessageToContext(msgData);
            }
        }
        else
        {
            if(requestbody["messages"][i]["role"]=="user")
            {
            if(requestbody["messages"][i]["role"]=="user")
            {
                std::string user_content = getContentAsString(requestbody["messages"][i]["content"]);
                if (session.requestmessage.empty()) {
                    session.requestmessage = user_content;
                } else {
                    session.requestmessage += user_content;
                }
            }
            }
        }
    }
    
       
    session.last_active_time = time(NULL);
    LOG_INFO<<__FILE__<<":"<<__FUNCTION__<<":"<<__LINE__<<"生成session_st完成";
    LOG_INFO << "session_st message_context: " << Json::FastWriter().write(session.message_context);
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
