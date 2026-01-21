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
#include <list>
using namespace drogon;
static const int SESSION_EXPIRE_TIME = 86400; //24小时，单位秒数,会话过期时间
//static const int SESSION_MAX_MESSAGES = 4; //上下文会话最大消息条数,一轮两条
// 图片信息结构
struct ImageInfo {
  std::string base64Data;      // base64编码的图片数据
  std::string mediaType;       // 图片类型如 image/png, image/jpeg
  std::string uploadedUrl;     // 上传后的图片URL
  int width = 0;
  int height = 0;
};

struct session_st
{
  std::string preConversationId="";
  std::string curConversationId="";
  std::string contextConversationId="";
  std::string apiChatinfoConversationId="";
  time_t last_active_time=0;
  std::string selectapi="";
  std::string selectmodel="";
  std::string systemprompt="";
  Json::Value message_context=Json::Value(Json::arrayValue);  // 存储上下文的一个JSON数组
  int contextlength=0;
  bool contextIsFull=false;
  std::string requestmessage="";
  std::vector<ImageInfo> requestImages;  // 当前请求中的图片列表
  Json::Value responsemessage;
  Json::Value client_info;
  void clearMessageContext()
  {
    message_context.clear();
  }
  void addMessageToContext(const Json::Value& message)
  {
    /*
    if(message_context.size() >= SESSION_MAX_MESSAGES)
    {
      message_context.pop_front();
    }
    */
    message_context.append(message);
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
    std::unordered_map<std::string, std::string> context_map;//上下文会话id与会话id的映射
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
    Json::Value generateJsonbySession(const session_st& session,bool contextIsFull);
};
#endif  