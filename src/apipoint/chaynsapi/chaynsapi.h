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

const int MAX_RETRIES = 100;  // 最大重试次数
const int BASE_DELAY = 300;  // 最大重试间隔（豪秒）
// ... existing code ...

std::string generateGuid();

using namespace std;

class Chaynsapi:public APIinterface
{
    public:
        static void* createApi();
        void postChatMessage(session_st& session);
        void checkAlivableTokens();
        void checkModels();
        Json::Value getModels();
        void init();
        ~Chaynsapi();
        void afterResponseProcess(session_st& session);
        void eraseChatinfoMap(string ConversationId);

    private:
        DEClARE_RUNTIME(chaynsapi);
        map<string,Json::Value> modelInfoMap; //modelname:modelinfo
        Json::Value model_info_openai_format;//v1/models openai接口格式

        void loadModels();
        bool checkAlivableToken(string token);

        Chaynsapi();
};
#endif