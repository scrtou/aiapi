#ifndef NEXOSAPI_H
#define NEXOSAPI_H

#include <apipoint/APIinterface.h>
#include <apiManager/ApiFactory.h>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>

struct Accountinfo_st;

class nexosapi : public APIinterface
{
  public:
    static void* createApi();

    nexosapi();
    ~nexosapi() override;

    provider::ProviderResult generate(session_st& session) override;
    void checkAlivableTokens() override;
    void checkModels() override;
    Json::Value getModels() override;
    Json::Value getAccountQuota(const std::string& userName = "");
    void init() override;
    void afterResponseProcess(session_st& session) override;
    void eraseChatinfoMap(std::string conversationId) override;
    void transferThreadContext(const std::string& oldId, const std::string& newId) override;

  private:
    DEClARE_RUNTIME(nexosapi);

    struct ChatContext {
        std::string chatId;
        std::string accountUserName;
    };

    struct RuntimeModelData {
        std::unordered_map<std::string, std::string> mapping;
        Json::Value models{Json::objectValue};
        std::string defaultAlias;
        std::string defaultHandlerId;
    };

    struct ChatDataPayload {
        std::string raw;
        Json::Value decoded;
    };

    provider::ProviderResult requestChatCompletion(session_st& session);
    provider::ProviderError classifyHttpError(int httpStatus, const std::string& message) const;

    std::shared_ptr<Accountinfo_st> selectAccount(const session_st& session, bool& reuseExistingChat);
    std::shared_ptr<Accountinfo_st> selectAccount(
        const session_st& session,
        bool& reuseExistingChat,
        const std::set<std::string>& excludedUserNames
    );
    std::shared_ptr<Accountinfo_st> selectAnyAccount();
    std::shared_ptr<Accountinfo_st> selectAccountByUserName(const std::string& userName);
    ChatDataPayload fetchChatDataPayload(const std::string& cookies) const;
    RuntimeModelData fetchRuntimeModelData(const std::string& cookies);
    std::string buildUserPrompt(const session_st& session, bool useExistingChat) const;
    std::string ensureChatId(const session_st& session, const std::shared_ptr<Accountinfo_st>& account, bool reuseExistingChat);
    std::string createChatId(const std::string& cookies) const;
    std::string resolveHandlerId(const RuntimeModelData& runtimeModels, const std::string& requestedModel) const;
    std::string fetchLastMessageId(const std::string& chatId, const std::string& cookies) const;
    std::string sendChatRequest(
        const std::string& chatId,
        const std::string& handlerId,
        const std::string& userText,
        const std::string& lastMessageId,
        const std::string& cookies,
        int& httpStatus
    ) const;
    std::string buildMultipartBody(
        const std::string& boundary,
        const std::string& chatId,
        const Json::Value& data
    ) const;
    std::string extractTextFromSsePayload(const std::string& payload) const;
    std::string extractChatIdFromChatData(const std::string& body) const;
    bool isBudgetExceededResponse(int httpStatus, const std::string& body) const;
    void markAccountBudgetExceeded(const std::shared_ptr<Accountinfo_st>& account) const;
    Json::Value fetchWhoamiPayload(const std::string& cookies) const;
    std::string extractEmailFromWhoami(const Json::Value& whoamiPayload) const;
    Json::Value buildModelList(const RuntimeModelData& runtimeModels);
    Json::Value buildQuotaResponse(const std::shared_ptr<Accountinfo_st>& account, const ChatDataPayload& payload) const;

    std::string baseUrl_;
    mutable std::mutex modelMutex_;
    Json::Value modelListOpenAiFormat_{Json::objectValue};

    std::mutex chatMutex_;
    std::unordered_map<std::string, ChatContext> chatMap_;
};

#endif
