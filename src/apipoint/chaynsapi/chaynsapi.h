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
#include <vector>

const int MAX_RETRIES = 6000;  // 轮询最大重试次数
const int BASE_DELAY = 100;  // 轮询重试间隔（毫秒）
const int CONSECUTIVE_FAILS_BEFORE_SWITCH = 3;  // 连续失败n次后换账号
const int MAX_UPSTREAM_RETRIES = 4;  // 上游最大总重试次数（外层循环，每次创建新线程或换账号）
const int SAME_THREAD_RETRIES = 2;  // 同一线程上的最大重试次数（内层循环，在同一线程上重新发送消息）
// 上游错误文本列表从 config.json 的 custom_config.upstream_error_texts 加载

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

    // 上游错误文本列表，从 config.json 的 custom_config.upstream_error_texts 加载
    std::vector<std::string> m_upstreamErrorTexts;
};
#endif
