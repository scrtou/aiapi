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

    if (accountinfo->personId.empty()) {
        LOG_INFO << "personId is empty, attempting to fetch it.";
        auto client = HttpClient::newHttpClient("https://auth.chayns.net");
        auto request = HttpRequest::newHttpRequest();
        request->setMethod(HttpMethod::Get);
        request->setPath("/v2/userSettings");
        request->addHeader("Authorization", "Bearer " + accountinfo->authToken);
        auto [result, response] = client->sendRequest(request);
        if (result == ReqResult::Ok && response->statusCode() == k200OK) {
            auto jsonResp = response->getJsonObject();
            if (jsonResp) {
                if (jsonResp->isMember("personId")) {
                    accountinfo->personId = (*jsonResp)["personId"].asString();
                    LOG_INFO << "Successfully fetched personId: " << accountinfo->personId;
                } else {
                    LOG_ERROR << "personId not found in userSettings response JSON. Body: " << response->getBody();
                }
            } else {
                LOG_ERROR << "Failed to parse userSettings response as JSON object. Body: " << response->getBody();
            }
        } else {
            LOG_ERROR << "Failed to fetch userSettings. Status code: " << (response ? response->statusCode() : 0) << ", Body: " << (response ? std::string(response->getBody()) : "No response");
        }
    }

    if (accountinfo->personId.empty()) {
        LOG_ERROR << "personId is still empty after attempting to fetch. Aborting.";
        session.responsemessage["error"] = "Failed to obtain a valid personId";
        session.responsemessage["statusCode"] = 500;
        return;
    }
    
    // 1. 发送消息并创建会话
    Json::Value sendMessageRequest;
    Json::Value member1;
    member1["isAdmin"] = true;
    member1["personId"] = accountinfo->personId;
    sendMessageRequest["members"].append(member1);

    Json::Value member2;
    const auto& model_info = modelInfoMap[modelname];
    if (!model_info.isMember("personId") || !model_info["personId"].isString()) {
        LOG_ERROR << "personId is missing or not a string for model: " << modelname;
        session.responsemessage["error"] = "Internal server error: model configuration issue.";
        session.responsemessage["statusCode"] = 500;
        return;
    }
    member2["personId"] = model_info["personId"].asString();
    sendMessageRequest["members"].append(member2);
    sendMessageRequest["nerMode"] = "None";
    sendMessageRequest["priority"] = 0;
    sendMessageRequest["typeId"] = 8;
    Json::Value message;
    message["text"] = user_message;
    sendMessageRequest["messages"].append(message);

    auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
    auto reqSend = HttpRequest::newHttpJsonRequest(sendMessageRequest);
    reqSend->setMethod(HttpMethod::Post);
    reqSend->setPath("/intercom-backend/v2/thread?forceCreate=true");
    reqSend->addHeader("Authorization", "Bearer " + accountinfo->authToken);
    LOG_INFO << "Chaynsapi request body: " << sendMessageRequest.toStyledString();

    string threadId;
    string lastMessageTime;
    string userAuthorId;

    auto sendResult = client->sendRequest(reqSend);
    if (sendResult.first != ReqResult::Ok) {
        LOG_ERROR << "Failed to send message";
        session.responsemessage["error"] = "Failed to send message";
        session.responsemessage["statusCode"] = 500;
        return;
    }
    auto responseSend = sendResult.second;
    LOG_INFO << "Thread creation response body: " << std::string(responseSend->getBody());
    if (responseSend->statusCode() == k200OK || responseSend->statusCode() == k201Created) {
        auto jsonResp = responseSend->getJsonObject();
        if (jsonResp && jsonResp->isMember("id")) {
            threadId = (*jsonResp)["id"].asString();
            if (jsonResp->isMember("messages") && (*jsonResp)["messages"].isArray() && (*jsonResp)["messages"].size() > 0) {
                const auto& firstMessage = (*jsonResp)["messages"][0];
                if (firstMessage.isMember("creationTime") && firstMessage["creationTime"].isString()) {
                    lastMessageTime = firstMessage["creationTime"].asString();
                }
            }
            if (jsonResp->isMember("members") && (*jsonResp)["members"].isArray()) {
                for (const auto& member : (*jsonResp)["members"]) {
                    if (member.isMember("personId") && member["personId"].asString() == accountinfo->personId) {
                        if (member.isMember("id") && member["id"].isString()) {
                            userAuthorId = member["id"].asString();
                        }
                        LOG_INFO << "Found userAuthorId: " << userAuthorId;
                        break;
                    }
                }
            }
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
        LOG_INFO << "Polling URL: " << "https://cube.tobit.cloud" << getPath;
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
            if (jsonResp && jsonResp->isArray() && !jsonResp->empty()) {
                // 查找来自非用户的最新消息
                for (int i = jsonResp->size() - 1; i >= 0; --i) {
                    const auto& message = (*jsonResp)[i];
                    if (message.isMember("author") && message["author"].isMember("id") &&
                        message["author"]["id"].asString() != userAuthorId &&
                        message.isMember("typeId") && message["typeId"].asInt() == 1) {
                        if (message.isMember("text") && message["text"].isString()) {
                            response_message = message["text"].asString();
                        }
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
