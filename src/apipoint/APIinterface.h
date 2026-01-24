#ifndef APIINTERFACE_H
#define APIINTERFACE_H
#include <string>
#include <map>
#include "../sessionManager/Session.h"
#include "ProviderResult.h"

using namespace std;

struct modelInfo
{
    string modelName;
    bool status=true;
};

class APIinterface
{
    public:
    virtual ~APIinterface() = default;
    
    /**
     * @brief 发送聊天消息（旧接口，写入 session.responsemessage）
     * @deprecated 建议使用 generate() 替代
     */
    virtual void postChatMessage(session_st& session) = 0;
    
    /**
     * @brief 发送聊天消息并返回结构化结果（新接口）
     *
     * 默认实现调用旧的 postChatMessage 并转换结果，
     * Provider 可以重写此方法以直接返回 ProviderResult。
     *
     * @param session 会话状态
     * @return ProviderResult 结构化结果
     */
    virtual provider::ProviderResult generate(session_st& session) {
        // 默认实现：调用旧接口并转换结果
        postChatMessage(session);
        return provider::ProviderResult::fromLegacyResponse(
            session.responsemessage.get("message", "").asString(),
            session.responsemessage.get("statusCode", 500).asInt()
        );
    }
    
    virtual void checkAlivableTokens() = 0;
    virtual void checkModels() = 0;
    virtual Json::Value getModels() = 0;
    virtual void init() = 0;
    virtual void afterResponseProcess(session_st& session) = 0;
    virtual void eraseChatinfoMap(std::string ConversationId) = 0;
    virtual void transferThreadContext(const std::string& oldId, const std::string& newId) = 0;
    
    map<string,modelInfo> ModelInfoMap;
};

#endif // APIINTERFACE_H