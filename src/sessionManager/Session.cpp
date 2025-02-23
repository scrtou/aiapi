#include "Session.h"
#include <time.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include "chaynsapi.h"
#include <apiManager/ApiManager.h>
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
    session = session_map[ConversationId];   
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
    LOG_INFO << "根据请求消息生成ConversationId: " << tempConversationId;
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
    //LOG_DEBUG << "生成ConversationId使用的数据: " << Json::FastWriter().write(keyData);
        return generateSHA256(Json::FastWriter().write(keyData));
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
    session.requestmessage.clear();
    session.responsemessage.clear();

    session.last_active_time=time(nullptr);
    std::string newConversationId;
    newConversationId=chatSession::getInstance()->generateConversationKey(
    chatSession::getInstance()->generateJsonbySession(session,session.contextIsFull)
    );
    session.preConversationId=session.curConversationId;    
    session.curConversationId = newConversationId;
    addSession(newConversationId,session);
    delSession(session.preConversationId);

    //如果上下文未满，则更新上下文长度,生成新的contextConversationId
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
        clientInfo["ip"] = req->getPeerAddr().toIp();
        
        // 获取User-Agent
        auto userAgent = req->getHeader("User-Agent");
        if (!userAgent.empty()) {
            clientInfo["user_agent"] = userAgent;
        }
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

        return clientInfo;
}
void chatSession::clearExpiredSession()
{
    LOG_INFO << "开始清除过期会话，当前会话数量:" << session_map.size();
    time_t now = time(nullptr);
    //遍历session_map,删除过期会话
    for(auto it = session_map.begin(); it != session_map.end();)
    {
        if (now - it->second.last_active_time > SESSION_EXPIRE_TIME) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                it = session_map.erase(it);
            }
            if(it->second.selectapi=="chaynsapi")
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ApiManager::getInstance().getApiByApiName(it->second.selectapi)->eraseChatinfoMap(it->first);
            }
        } else {
            ++it;
        }
    }
    LOG_INFO << "清除过期会话完成，剩余会话数量:" << session_map.size();
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
    
    for(int i = 0; i < requestbody["messages"].size()-1; i++)
    {
         if(requestbody["messages"][i]["role"] == "system")
            {
                session.systemprompt = requestbody["messages"][i]["content"].asString();
                continue;
            }   
        Json::Value msgData;
        msgData["role"] = requestbody["messages"][i]["role"].asString();
        msgData["content"] = Json::FastWriter().write(requestbody["messages"][i]["content"]);
        session.addMessageToContext(msgData);
    }
    session.requestmessage = Json::FastWriter().write(requestbody["messages"][requestbody["messages"].size()-1]["content"]);
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
