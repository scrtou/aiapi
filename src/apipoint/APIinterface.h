#ifndef APIINTERFACE_H
#define APIINTERFACE_H
#include <string>
#include <map>
#include "sessionManager/core/Session.h"
#include "ProviderResult.h"

using std::map;
using std::string;

struct modelInfo
{
    std::string modelName;
    bool status=true;
};

class APIinterface
{
    public:
    virtual ~APIinterface() = default;

    /**
     * @brief 发送聊天消息并返回结构化结果（主接口）
     *
     * @param session 会话状态
     * @return ProviderResult 结构化结果
     */
    virtual provider::ProviderResult generate(session_st& session) = 0;
    
    virtual void checkAlivableTokens() = 0;
    virtual void checkModels() = 0;
    virtual Json::Value getModels() = 0;
    virtual void init() = 0;
    virtual void afterResponseProcess(session_st& session) = 0;
    virtual void eraseChatinfoMap(std::string ConversationId) = 0;
    virtual void transferThreadContext(const std::string& oldId, const std::string& newId) = 0;
    
    map<string,modelInfo> ModelInfoMap;
};

#endif
