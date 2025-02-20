#ifndef CHAYNSAPI_H
#define CHAYNSAPI_H
#include "APIinterface.h"
#include <accountManager/accountManager.h>
#include "../../sessionManager/Session.h"
#include "../../apiManager/ApiFactory.h"
#include <list>
#include <map>
#include <random>
#include <sstream>
#include <iomanip>

const int MAX_RETRIES = 30;  // 最大重试次数
const int BASE_DELAY = 300;  // 最大重试间隔（豪秒）
// ... existing code ...

std::string generateGuid();

using namespace std;

struct chatinfo_st
{
   string threadid="";
   string usermessageid="";
   string messagecreatetime="";
   int modelbotid=-1;
   string picuserfilepath="";
    int status=-1; //0:预留，1:占用；2：空闲
   shared_ptr<Accountinfo_st> accountinfo=nullptr;
};

class Chaynsapi:public APIinterface
{
    public:
        static void* createApi();
        void postChatMessage(session_st& session);
        void checkAlivableTokens();
        void checkModels();
        Json::Value getModels();
        //创建聊天线程
        void init();
         ~Chaynsapi();
         void afterResponseProcess(session_st& session);
         void eraseChatinfoMap(string ConversationId);

    private:
        DEClARE_RUNTIME(chaynsapi);
        map<string,chatinfo_st> chatinfoMap; //ConversationId:chatinfo_st
        map<string,list<chatinfo_st>> chatinfoPollMap; //modelb:chatinfo_st
        map<string,Json::Value> modelNameIdMap; //modelname:modelid
        Json::Value model_info;
        std::mutex chatinfoPollMap_mutex;
        std::mutex chatinfoMap_mutex;

        void loadUsertokenlist();
        void loadChatinfoPollMap();
        void loadModels();
        bool checkAlivableToken(string token);
         void createChatThread( string modelname,shared_ptr<Accountinfo_st> accountinfo,string& threadid,string& usermessageid);
        void createChatThread(string modelname,chatinfo_st& chatinfo);
        //发送消息
        void sendMessage(shared_ptr<Accountinfo_st> accountinfo,string threadid,string usermessageid,string message,string& creationTime);
        //获取消息
        void getMessage(shared_ptr<Accountinfo_st> accountinfo,string threadid,string usermessageid,string &creationTime,string& response_message,int& response_statusCode);

        Chaynsapi();
       
        string modelUrl="https://intercom.tobit.cloud/api/v1/models";
};  
#endif