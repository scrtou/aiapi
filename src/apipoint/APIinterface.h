#ifndef APIINTERFACE_H
#define APIINTERFACE_H
#include <string>
#include <map>
#include "../sessionManager/Session.h"
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
    virtual void postChatMessage(session_st& session) = 0;
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