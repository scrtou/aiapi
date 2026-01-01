#include <drogon/drogon.h>
#include <chaynsapi.h>
#include <../../apiManager/Apicomn.h> 
#include <unistd.h>
IMPLEMENT_RUNTIME(chaynsapi,Chaynsapi);
using namespace drogon;

Chaynsapi::Chaynsapi()
{
   
}

Chaynsapi::~Chaynsapi()
{
}

void Chaynsapi::init()
{
    loadModels();
}

void Chaynsapi::postChatMessage(session_st& session)
{
    LOG_INFO << "Chaynsapi::postChatMessage";
    string modelname = session.selectmodel;
    string user_message = session.requestmessage;

    shared_ptr<Accountinfo_st> accountinfo = nullptr;
    AccountManager::getInstance().getAccount("chaynsapi", accountinfo);
    if (accountinfo == nullptr || !accountinfo->tokenStatus)
    {
        LOG_ERROR << "Failed to get a valid account for chaynsapi";
        session.responsemessage["error"] = "No valid account available";
        session.responsemessage["statusCode"] = 500;
        return;
    }

    // 1. 发送消息并创建会话
    Json::Value sendMessageRequest;
    sendMessageRequest["members"][0]["isAdmin"] = true;
    sendMessageRequest["members"][0]["personId"] = accountinfo->personId;
    sendMessageRequest["members"][1]["personId"] = modelInfoMap[modelname]["personId"].asString();
    sendMessageRequest["nerMode"] = "None";
    sendMessageRequest["priority"] = 0;
    sendMessageRequest["typeId"] = 8;
    sendMessageRequest["messages"][0]["text"] = user_message;

    auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
    auto reqSend = HttpRequest::newHttpJsonRequest(sendMessageRequest);
    reqSend->setMethod(HttpMethod::Post);
    reqSend->setPath("/intercom-backend/v2/thread?forceCreate=true");
    reqSend->addHeader("Authorization", "Bearer " + accountinfo->authToken);

    string threadId;
    string lastMessageTime;

    auto sendResult = client->sendRequest(reqSend);
    if (sendResult.first != ReqResult::Ok) {
        LOG_ERROR << "Failed to send message";
        session.responsemessage["error"] = "Failed to send message";
        session.responsemessage["statusCode"] = 500;
        return;
    }
    auto responseSend = sendResult.second;
    if (responseSend->statusCode() == k200OK) {
        auto jsonResp = responseSend->getJsonObject();
        if (jsonResp && jsonResp->isMember("id")) {
            threadId = (*jsonResp)["id"].asString();
            lastMessageTime = (*jsonResp)["messages"][0]["creationTime"].asString();
        }
    } else {
        LOG_ERROR << "Send message failed with status " << responseSend->statusCode();
        session.responsemessage["error"] = "Failed to create thread";
        session.responsemessage["statusCode"] = responseSend->statusCode();
        return;
    }

    if (threadId.empty()) {
        LOG_ERROR << "Failed to get threadId from response";
        session.responsemessage["error"] = "Failed to get threadId";
        session.responsemessage["statusCode"] = 500;
        return;
    }

    // 2. 轮询获取结果
    string response_message;
    int response_statusCode = 204; // No Content

    for (int retry = 0; retry < MAX_RETRIES; ++retry) {
        auto reqGet = HttpRequest::newHttpRequest();
        reqGet->setMethod(HttpMethod::Get);
        string getPath = "/intercom-backend/v2/thread/" + threadId + "/message?take=1000&afterDate=" + lastMessageTime;
        reqGet->setPath(getPath);
        reqGet->addHeader("Authorization", "Bearer " + accountinfo->authToken);

        auto getResult = client->sendRequest(reqGet);
        if (getResult.first != ReqResult::Ok) {
            LOG_ERROR << "Failed to get message on retry " << retry;
            continue;
        }
        auto responseGet = getResult.second;
        if (responseGet->statusCode() == k200OK) {
            auto jsonResp = responseGet->getJsonObject();
            if (jsonResp && jsonResp->isArray() && jsonResp->size() > 1) {
                // 查找来自非用户的最新消息
                for (int i = jsonResp->size() - 1; i >= 0; --i) {
                    const auto& message = (*jsonResp)[i];
                    if (message.isMember("author") && message["author"].isMember("id") &&
                        message["author"]["id"].asString() != accountinfo->personId) {
                        response_message = message["text"].asString();
                        response_statusCode = 200;
                        goto found;
                    }
                }
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY));
    }

found:
    if (response_statusCode == 200) {
        session.responsemessage["message"] = response_message;
    } else {
        LOG_ERROR << "Failed to get message after " << MAX_RETRIES << " retries.";
        session.responsemessage["error"] = "Timeout waiting for response";
    }
    session.responsemessage["statusCode"] = response_statusCode;
}

void Chaynsapi::checkAlivableTokens()
{

}
bool Chaynsapi::checkAlivableToken(string token)
{
    auto client = HttpClient::newHttpClient("https://auth.chayns.net");
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(HttpMethod::Get);
    request->setPath("/v2/userSettings");
    request->addHeader("Authorization", "Bearer " + token);
    auto [result, response] = client->sendRequest(request);
    LOG_DEBUG << "checkAlivableToken response: " << response->getStatusCode();
    if(response->getStatusCode()!=200)
    {
        return false;
    }
    return true;
}
void Chaynsapi::checkModels()
{

}
void Chaynsapi::loadModels()
{
    auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
    auto request = HttpRequest::newHttpRequest();
    
    request->setMethod(HttpMethod::Get);
    request->setPath("/chayns-ai-chatbot/nativeModelChatbot");
    
    auto [result, response] = client->sendRequest(request);
    
    if (result != ReqResult::Ok || response->statusCode() != k200OK) {
        LOG_ERROR << "Failed to fetch models from API";
        return;
    }
    
    Json::Value api_models;
    Json::Reader reader;
    if (!reader.parse(string(response->getBody()), api_models)) {
        LOG_ERROR << "Failed to parse models API response";
        return;
    }
    
    model_info_openai_format["object"] = "list";
    model_info_openai_format["data"] = Json::Value(Json::arrayValue);

    for (const auto& model : api_models) {
        string showName = model.get("showName", "").asString();
        if (!showName.empty()) {
            modelInfoMap[showName] = model;

            Json::Value tmp_model_info;
            tmp_model_info["id"] = showName;
            tmp_model_info["object"] = "model";
            tmp_model_info["created"] = 1626777600;
            tmp_model_info["owned_by"] = "chayns";
            model_info_openai_format["data"].append(tmp_model_info);
        }
    }
    
    LOG_INFO << "Chayns NativeModelChatbot models successfully loaded: " << modelInfoMap.size() << " models.";
}

std::string generateGuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++) {
        ss << dis(gen);
    }
    ss << "-";
    for (int i = 0; i < 4; i++) {
        ss << dis(gen);
    }
    ss << "-4";  // Version 4
    for (int i = 0; i < 3; i++) {
        ss << dis(gen);
    }
    ss << "-";
    ss << dis2(gen);  // Variant
    for (int i = 0; i < 3; i++) {
        ss << dis(gen);
    }
    ss << "-";
    for (int i = 0; i < 12; i++) {
        ss << dis(gen);
    }
    return ss.str();
}
void Chaynsapi::afterResponseProcess(session_st& session)
{
   // No longer needed with the new stateless API
}
void Chaynsapi::eraseChatinfoMap(string ConversationId)
{
    // No longer needed with the new stateless API
}
Json::Value Chaynsapi::getModels()
{
   return model_info_openai_format;
}
void* Chaynsapi::createApi()
{
    Chaynsapi* chaynsapi=new Chaynsapi();
    chaynsapi->init();
    return chaynsapi;
}
