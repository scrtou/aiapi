#ifndef SESSION_H
#define SESSION_H
#include <string>
#include <map>
#include <unordered_map>
#include <mutex>
#include<drogon/drogon.h>
#include <json/json.h>
#include <sstream>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/err.h>
#include <iomanip>
#include <stdexcept>
#include <deque>
using namespace drogon;
static const int SESSION_EXPIRE_TIME = 86400; //24小时，单位秒数,会话过期时间
static const int SESSION_MAX_MESSAGES = 4; //上下文会话最大消息条数,一轮两条
struct session_st
{
  std::string preConversationId="";
  std::string curConversationId="";
  time_t last_active_time=0;
  std::string selectapi="";
  std::string selectmodel="";
  std::string systemprompt="";
  std::deque<Json::Value> message_context;  // 存储上下文的双端队列，每个元素是一个JSON对象
  std::string requestmessage="";
  Json::Value responsemessage;
  Json::Value client_info;
  void clearMessageContext()
  {
    message_context.clear();
  }
  void addMessageToContext(const Json::Value& message)
  {
    if(message_context.size() >= SESSION_MAX_MESSAGES)
    {
      message_context.pop_front();
    }
    message_context.push_back(message);
  }
};

class chatSession
{
  private:
    chatSession();
    ~chatSession();
    static chatSession *instance;
    std::mutex mutex_;
    std::unordered_map<std::string, session_st> session_map;
public:
    static chatSession *getInstance()
    {
        static chatSession instance;
        return &instance;
    }
    void addSession(const std::string &ConversationId,session_st &session);
    void delSession(const std::string &ConversationId);
    void getSession(const std::string &ConversationId, session_st &session);
    void updateSession(const std::string &ConversationId,session_st &session);
    void clearExpiredSession();
    void startClearExpiredSession();
    session_st& createNewSessionOrUpdateSession(session_st& session);
    static std::string generateConversationKey(
        const Json::Value& keyData
    );
    void coverSessionresponse(session_st& session);
    static std::string generateSHA256(const std::string& input);
    static Json::Value getClientInfo(const HttpRequestPtr &req);

    bool sessionIsExist(const std::string &ConversationId);
    session_st gennerateSessionstByReq(const HttpRequestPtr &req);
    Json::Value generateJsonbySession(const session_st& session);
};
#endif  