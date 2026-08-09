#ifndef CHAYNSAPI_H
#define CHAYNSAPI_H
#include <accountManager/accountManager.h>
#include "domain/model/SessionData.h"
#include "../../apiManager/ApiFactory.h"
#include <list>
#include <map>
#include <random>
#include <sstream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <cstdint>
#include <shared_mutex>
#include "ChaynsModelCatalog.h"
#include "ChaynsMessageCorrelation.h"
#include "ChaynsPollingPolicy.h"
#include "ChaynsHttpTransport.h"
#include "ChaynsClock.h"

using std::list;
using std::map;
using std::string;

const int BASE_DELAY = 100;  // 轮询重试间隔（毫秒）
const int CONSECUTIVE_FAILS_BEFORE_SWITCH = 3;  // 连续失败n次后换账号
const int MAX_UPSTREAM_RETRIES = 4;  // 上游最大总重试次数（外层循环，每次创建新线程或换账号）
// 上游错误文本列表从配置 custom_config.upstream_error_texts 加载

std::string generateGuid();

class chaynsapi:public APIinterface
{
    public:
        static void* createApi();
        explicit chaynsapi(std::shared_ptr<chayns::IChaynsHttpTransport> transport);
        chaynsapi(std::shared_ptr<chayns::IChaynsHttpTransport> transport,
                  std::shared_ptr<chayns::IChaynsClock> clock);
        provider::ProviderResult generate(session_st& session) override;
        void postChatMessage(session_st& session);
        void checkAlivableTokens();
        void checkModels();
        Json::Value getModels();
        void init();
        ~chaynsapi();
        void afterResponseProcess(session_st& session);
        void eraseChatinfoMap(string ConversationId);
        void transferThreadContext(const std::string& oldId, const std::string& newId) override;

        // ThreadReaper 需要按台账行还原“同账号 + 同 Origin”再发删除请求，
        // 因此该入口必须公开：回收线程不持有 m_threadMap，只能靠 DB 行重建删除上下文。
        // 返回值即回收闭环的判据：false 表示上游仍可能残留，调用方应重试而非删台账行。
        bool deleteUpstreamThread(const std::string& accountUserName,
                                  const std::string& threadId,
                                  const std::string& origin,
                                  const std::string& referer);

    private:
        DEClARE_RUNTIME(chaynsapi);
        chayns::ModelCatalog m_modelCatalog;
        mutable std::shared_mutex m_modelCatalogMutex;
        std::mutex m_modelRefreshMutex;
        std::chrono::steady_clock::time_point m_modelsLoadedAt{};
        std::chrono::steady_clock::time_point m_lastModelRefreshAttempt{};

        bool loadModels(bool forceRefresh = false);
        bool findModel(const std::string& modelName, chayns::ModelDescriptor& model) const;
        bool checkAlivableToken(string token);
        // 上传图片到图片服务，返回上传后的 URL
        std::string uploadImageToService(const ImageInfo& image,
                                         const std::string& personId,
                                         const std::string& authToken,
                                         const std::string& accountUserName,
                                         const std::string& origin,
                                         const std::string& referer);

        chaynsapi();
        std::shared_ptr<chayns::IChaynsHttpTransport> m_transport;
        std::shared_ptr<chayns::IChaynsClock> m_clock;
    
    // 定义一个结构体保存线程上下文信息
    struct ThreadContext {
        std::string threadId;
        std::string userAuthorId; // 当前账号在该线程中的AuthorID，用于识别后续用户消息
        std::string agentAuthorId; // 上游模型在该线程中的AuthorID
        std::string accountUserName; // 创建该线程时使用的账户userName，用于后续请求使用相同账户
        std::string modelId; // 创建线程时所选模型，防止续聊时错误复用其它模型的线程
        std::string accountType;
        int threadTypeId = 8;
        std::int64_t workspaceUacId = 0;
        std::string origin;
        std::string referer;
        std::string lastRequestMessageId;
        std::string lastRequestCreationTime;
        std::string lastAssistantMessageId;
        Json::Value lastReasoningMessages = Json::Value(Json::arrayValue);
    };


    std::map<std::string, ThreadContext> m_threadMap;
    std::mutex m_threadMapMutex;

    // 上游错误文本列表，从配置 custom_config.upstream_error_texts 加载
    std::vector<std::string> m_upstreamErrorTexts;
};
#endif
