#ifndef CHAYNSAPI_H
#define CHAYNSAPI_H
#include <accountManager/accountManager.h>
#include "../../sessionManager/Session.h"
#include "../../apiManager/ApiFactory.h"
#include <list>
#include <map>
#include <random>
#include <sstream>
#include <iomanip>

const int MAX_RETRIES = 1000;  // 最大重试次数
const int BASE_DELAY = 300;  // 最大重试间隔（豪秒）

std::string generateGuid();

using namespace std;

class chaynsapi:public APIinterface
{
    public:
        static void* createApi();
        void postChatMessage(session_st& session);
        void checkAlivableTokens();
        void checkModels();
        Json::Value getModels();
        void init();
        ~chaynsapi();
        void afterResponseProcess(session_st& session);
        void eraseChatinfoMap(string ConversationId);
        void transferThreadContext(const std::string& oldId, const std::string& newId) override;

    private:
        DEClARE_RUNTIME(chaynsapi);
        map<string,Json::Value> modelInfoMap; //modelname:modelinfo
        Json::Value model_info_openai_format;//v1/models openai接口格式

        void loadModels();
        bool checkAlivableToken(string token);
        // 上传图片到 image-service，返回上传后的URL
        std::string uploadImageToService(const ImageInfo& image, const std::string& personId, const std::string& authToken);

        chaynsapi();
    
    // 定义一个结构体保存线程上下文信息
    struct ThreadContext {
        std::string threadId;
        std::string userAuthorId; // Bot在该线程中的AuthorID，用于轮询时过滤
        std::string accountUserName; // 创建该thread时使用的账户userName，用于后续请求使用相同账户
    };

    // 映射表: ConversationId -> ThreadContext
    std::map<std::string, ThreadContext> m_threadMap;
    std::mutex m_threadMapMutex;
};
#endif
