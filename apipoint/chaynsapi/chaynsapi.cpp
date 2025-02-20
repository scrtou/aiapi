
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
    //loadUsertokenlist();
    //loadChatinfoPollMap();
}

void Chaynsapi::loadUsertokenlist()
{
    /*
    //从配置中读取用户名，密码，token，userbotid，picuserrootpath
    auto customConfig = app().getCustomConfig();
    auto chaynsapiConfig = customConfig["chaynsapi"];
    auto username = chaynsapiConfig["username"].asString();
    auto passwd = chaynsapiConfig["passwd"].asString();
    auto userbotid = chaynsapiConfig["user_tobit_id"].asInt();
    auto bearer_token = chaynsapiConfig["bearer_token"].asString();
    LOG_INFO << "username: " << username;
    LOG_INFO << " passwd: " << passwd;
    LOG_INFO << " userbotid: " << userbotid;
    LOG_INFO << " bearer_token: " << bearer_token;
    shared_ptr<Accountinfo_st> accountinfo=make_shared<Accountinfo_st>();
    accountinfo->userName = username;
    accountinfo->passwd = passwd;
    accountinfo->userTobitId = userbotid;  
    usertokenst->auth_token = bearer_token;
    usertokenst->status=true;
    usertokenlist.push_back(usertokenst);
    LOG_INFO << "usertokenlist size: " << usertokenlist.size();
*/
}

void Chaynsapi::loadChatinfoPollMap()
{   
    LOG_INFO << " Chaynsapi::loadChatinfoPollMap";
    for(auto& tempnam:modelNameIdMap)
    {
        string modelname=tempnam.first;
        chatinfo_st chatinfo;
        createChatThread(modelname,chatinfo);
        if(chatinfo.threadid.empty())
        {
            LOG_ERROR << "%s Failed to create thread",modelname.c_str() ;
        }
        else
        {
            chatinfoPollMap[modelname].push_back(std::move(chatinfo));
        }
    }


    LOG_INFO << " Chaynsapi::loadChatinfoPollMap Successfully loaded " << chatinfoPollMap.size() << " chatinfo";
    /*
    for(auto& tempnam:chatinfoPollMap)
    {
        
        for(auto& chatinfo:tempnam.second)
        {
            LOG_INFO << "chatinfo: " << tempnam.first << " " << chatinfo.threadid << " " << chatinfo.usermessageid << " " << chatinfo.modelbotid << " " << chatinfo.status;
        }
    }    
    */
}
void Chaynsapi::createChatThread(string modelname,chatinfo_st& chatinfo)
{
    LOG_INFO << " Chaynsapi::createChatThread1";
    LOG_DEBUG << "modelname: " << modelname;
    shared_ptr<Accountinfo_st> accountinfo=nullptr;
    AccountManager::getInstance().getAccount("chaynsapi",accountinfo);
    if(accountinfo==nullptr||accountinfo->tokenStatus==false)
   {
       LOG_ERROR << "Failed to get account";
       return;
   }
   // LOG_INFO << "userbotid: " << accountinfo->userTobitId;
   // LOG_INFO << "auth_token: " << accountinfo->authToken;
   // LOG_INFO << "username: " << accountinfo->userName;
   // LOG_INFO << "passwd: " << accountinfo->passwd;

    string threadid,usermessageid;
    createChatThread(modelname,accountinfo,threadid,usermessageid);
    if(!threadid.empty())
    {
        chatinfo.threadid=threadid;
        chatinfo.usermessageid=usermessageid;
        chatinfo.modelbotid=modelNameIdMap[modelname]["tobit_id"].asInt();
        chatinfo.status=0;
        chatinfo.accountinfo=accountinfo;
    }
    else
    {
        LOG_ERROR << "Failed to create thread";
    }

}
void Chaynsapi::createChatThread(string modelname,shared_ptr<Accountinfo_st> accountinfo,string& threadid,string& usermessageid)
{
    LOG_INFO << " Chaynsapi::createChatThread";

    // 构建JSON结构
    Json::Value root;
    root["forceCreate"] = true;
    root["tag"] = "Agent";
    
    Json::Value members(Json::arrayValue);
    Json::Value member1;
    member1["tobitId"] = modelNameIdMap[modelname]["tobit_id"].asInt();  
    members.append(member1);
    
    Json::Value member2;    
    member2["tobitId"] = accountinfo->userTobitId;
    members.append(member2);
    
    root["thread"]["anonymizationForAI"] = false;
    root["thread"]["members"] = members;
    root["thread"]["typeId"] = 8;

    Json::FastWriter writer;
    std::string requestBody = writer.write(root);
    
    // 创建HTTP客户端
    auto client = HttpClient::newHttpClient("https://intercom.tobit.cloud");
    auto req = HttpRequest::newHttpRequest();
    
    // 设置请求
    req->setMethod(HttpMethod::Post);
    req->setPath("/api/thread");
    req->setBody(requestBody);
    
    // 设置请求头
    req->setContentTypeString("application/json");
    req->addHeader("Authorization", accountinfo->authToken);
    req->addHeader("Accept", "*/*");
    /*
    LOG_INFO << "=== Full Request Details ===";
    LOG_INFO << "URL: https://intercom.tobit.cloud/api/thread";
    LOG_INFO << "Method: POST";
    LOG_INFO << "=== Request Headers ===";
    LOG_INFO << "Content-Type: application/json";
    LOG_INFO << "Authorization: " << accountinfo->authToken;
    */
    //LOG_INFO << "Accept: */*";
    
    LOG_DEBUG << "=== Request Body ===";
    LOG_DEBUG << requestBody;
    
    // 发送请求
    auto [result, response] = client->sendRequest(req);
    
    if (result != ReqResult::Ok) {
        LOG_ERROR << "Failed to send request";
        return;
    }
    
    // 获取响应码
    int statusCode = response->getStatusCode();
    std::string responseBody = std::string(response->getBody());  // 显式转换
    
    LOG_DEBUG << "=== Response ===";
    LOG_DEBUG << "Status Code: " << statusCode;
    LOG_DEBUG << "=== Response Body ===";
    LOG_DEBUG << responseBody;
    
    // 解析响应
    Json::Value resp_json;
    Json::Reader reader;
    if(reader.parse(responseBody, resp_json)) {
        if (resp_json.isMember("thread") && resp_json["thread"].isMember("id")) {
            threadid = resp_json["thread"]["id"].asString();
            if (resp_json["thread"].isMember("members") && resp_json["thread"]["members"].size() > 1) {
                usermessageid = resp_json["thread"]["members"][1]["id"].asString();
            }
        }
    } else {
        LOG_ERROR << "Failed to parse response JSON";
    }
    
    LOG_INFO << "threadid: " << threadid;
    LOG_INFO << "usermessageid: " << usermessageid;
    if (threadid.empty()) {
        LOG_ERROR << "Failed to create thread";
    }
}

void Chaynsapi::postChatMessage(session_st& session)
{
    string postmessages;
    LOG_INFO << "Chaynsapi::postChatMessage";
    //先根据client ID获取模型名称
    string modelname=session.selectmodel;
    string ConversationId=session.curConversationId;
    //尝试从chatinfoMap中获取chatinfo_st信息
    chatinfo_st chatinfo;
    if(chatinfoMap.find(ConversationId)!=chatinfoMap.end())
    {
        std::lock_guard<std::mutex> mapLock(chatinfoMap_mutex);
        chatinfo=chatinfoMap[ConversationId];
        LOG_INFO << "从chatinfoMap中获取chatinfo_st信息";
    }
    else
    {
        //从缓存池中根据模型名称获取chatinfo_st 
        LOG_INFO << "从缓存池获取chatinfo_st信息";
        if(!chatinfoPollMap[modelname].empty())   
        {
            std::lock_guard<std::mutex> pollLock(chatinfoPollMap_mutex);
            chatinfo=chatinfoPollMap[modelname].front();
            chatinfoPollMap[modelname].pop_front();
            std::lock_guard<std::mutex> mapLock(chatinfoMap_mutex);
            chatinfoMap[session.curConversationId]=chatinfo;
        }
        else
        {
            LOG_ERROR << "缓存池中已没有chatinfo_st信息";
            createChatThread(modelname,chatinfo);
            if(chatinfo.threadid.empty())
            {
                LOG_ERROR << "%s Failed to create thread",modelname.c_str() ;
                return;
            }
            else
            {
                std::lock_guard<std::mutex> mapLock(chatinfoMap_mutex);
                chatinfoMap[session.curConversationId]=chatinfo;
            }
        }
        if(session.systemprompt.empty())
        {
            session.systemprompt=model_info["description"].asString();  
        }
        postmessages="忘记你之前的所有身份，接下来你将扮演的角色描述如下:"+session.systemprompt+"\n\n";
        if(!session.message_context.empty())
        {
            postmessages+="以下是我和你的历史对话:\n\n";
            for(auto& message:session.message_context)
            {
                postmessages+=message["role"].asString()+":"+message["content"].asString()+"\n\n";
            }
        }
        postmessages+=string("现在开始正式对话,请根据我的问题给出回答,下面是我的问题:\n\n");
        //创建一条该模型的缓存
        thread t1([this,modelname](){
            chatinfo_st chatinfo;
            this->createChatThread(modelname,chatinfo);
            std::lock_guard<std::mutex> pollLock(chatinfoPollMap_mutex);
            chatinfoPollMap[modelname].push_back(chatinfo);
        });
        t1.detach();
    }
    LOG_INFO << " 已获取chatinfo_st信息";
    LOG_DEBUG <<"ConversationId: "<< ConversationId << "chatinfo: " << chatinfo.threadid << " " << chatinfo.usermessageid << " " << chatinfo.modelbotid << " " << chatinfo.status;
    if(chatinfo.threadid.empty() )
    {
        LOG_ERROR << "Failed to create thread";
        return;
    }
    if(chatinfo.accountinfo==nullptr||chatinfo.accountinfo->tokenStatus==false)
    {
        LOG_ERROR << "Failed to get account";
        return;
    }
    string user_message=postmessages+session.requestmessage;
    LOG_INFO << "user_message: " << user_message;
    

    string creationTime;
    sendMessage(chatinfo.accountinfo,chatinfo.threadid,chatinfo.usermessageid,user_message,creationTime);
    if(creationTime.empty())
    {
        LOG_ERROR << "Failed to send message";
        return;
    }
    //更新chatinfo_st信息
    chatinfoMap[ConversationId].status=1;
    chatinfoMap[ConversationId].messagecreatetime=creationTime;

    //获取消息
    string response_message;
    int response_statusCode;
    getMessage(chatinfo.accountinfo,chatinfo.threadid,chatinfo.usermessageid,creationTime,response_message,response_statusCode);
    //更新chatinfo_st信息
    chatinfoMap[ConversationId].status=2;
    chatinfoMap[ConversationId].messagecreatetime=creationTime;
    //返回消息
    session.responsemessage["message"]=response_message;
    session.responsemessage["statusCode"]=response_statusCode;
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
    request->addHeader("Authorization", token);
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

    LOG_INFO << "Models API called";
    
    // 创建HTTP客户端
    auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
    auto request = HttpRequest::newHttpRequest();
    
    // 设置请求
    request->setMethod(HttpMethod::Get);
    request->setPath("/ai-proxy/v1/models");
    
    // 发送请求
    auto [result, response] = client->sendRequest(request);
    
    if (result != ReqResult::Ok) {
        LOG_ERROR << "Failed to fetch models from API";
        Json::Value error;
        error["error"]["message"] = "Failed to fetch models";
        error["error"]["type"] = "api_error";
        LOG_ERROR << "Failed to fetch models from API";
        return;
    }
    
    // 解析API响应
    Json::Value api_models;
    Json::Reader reader;
    std::string body = std::string(response->getBody());  // 显式转换
    if (!reader.parse(body, api_models)) {
        LOG_ERROR << "Failed to parse API response";
        Json::Value error;
        error["error"]["message"] = "Failed to parse models response";
        error["error"]["type"] = "api_error";
        LOG_ERROR << "Failed to parse API response";
        return;
    }
    
    // 构建响应
    model_info["object"] = "list";
    model_info["data"] = Json::Value(Json::arrayValue);
    
    // 过滤和转换可用的模型
    for (const auto& model : api_models) {
        if (model.get("isAvailable", false).asBool()) {
            Json::Value tmp_model_info;
            tmp_model_info["id"] = model.get("modelName", "");
            tmp_model_info["name"] = model.get("showName", "");
            tmp_model_info["description"] = model.get("showName", "").asString() + " model";
            tmp_model_info["context_length"] = 32000;  // 默认上下文长度
            tmp_model_info["tobit_id"] = model.get("tobitId", 0);
            // LOG_INFO << "id: " << model_info["id"].asString()<<" tobit_id: "<<model_info["tobit_id"].asInt();
            modelNameIdMap[tmp_model_info["id"].asString()]=tmp_model_info;
        //返回v1/models openai接口格式
        /*
        data": [
        {
            "id": model,
            "object": "model",
            "created": 1626777600,  # 假设创建时间为固定值，实际使用时应从数据源获取
            "owned_by": "example_owner",  # 假设所有者为固定值，实际使用时应从数据源获取
            "permission": [
                {
                    "id": "modelperm-LwHkVFn8AcMItP432fKKDIKJ",  # 假设权限ID为固定值，实际使用时应从数据源获取
                    "object": "model_permission",
                    "created": 1626777600,  # 假设创建时间为固定值，实际使用时应从数据源获取
                    "allow_create_engine": True,
                    "allow_sampling": True,
                    "allow_logprobs": True,
                    "allow_search_indices": False,
                    "allow_view": True,
                    "allow_fine_tuning": False,
                    "organization": "*",
                    "group": None,
                    "is_blocking": False
                }
            ],
            "root": model,
            "parent": None
        } for model in lst_models
        ],
        */
        Json::Value tmpv1_model_info;
        tmpv1_model_info["id"]=tmp_model_info["id"].asString();
        tmpv1_model_info["object"]="model";
        tmpv1_model_info["created"]=1626777600;
        tmpv1_model_info["owned_by"]="example_owner";
        tmpv1_model_info["permission"]=Json::Value(Json::arrayValue);
        model_info["data"].append(tmpv1_model_info);

        }
    }
    
    LOG_INFO << " Chaynsapi modles Successfully loaded " << model_info["data"].size() << " models from API" << " ModelNameIdMap size: " << modelNameIdMap.size();
   
}

void Chaynsapi::sendMessage(shared_ptr<Accountinfo_st> accountinfo,string threadid,string usermessageid,string message,string& creationTime)
{
    LOG_INFO << "Chaynsapi::sendMessage";
    
    // 构建JSON结构
    Json::Value json;
    // 创建 author 对象
    Json::Value author;
    author["tobitId"] = accountinfo->userTobitId;
    json["author"] = author;

// 创建 message 对象
    Json::Value messagejson;
    messagejson["text"] = message;
    messagejson["typeId"] = 1;
    messagejson["meta"] = Json::Value(Json::objectValue);  // 创建空对象
    messagejson["guid"] = generateGuid();
    json["message"] = messagejson;

    Json::FastWriter writer;
    std::string requestBody = writer.write(json);
    
    // 创建HTTP客户端
    auto client = HttpClient::newHttpClient("https://intercom.tobit.cloud");
    auto req = HttpRequest::newHttpRequest();
    
    // 设置请求
    req->setMethod(HttpMethod::Post);
    req->setPath("/api/thread/" + threadid + "/message");
    req->setBody(requestBody);
    
    // 设置请求头
    req->setContentTypeString("application/json");
    req->addHeader("Authorization", accountinfo->authToken);
    req->addHeader("Accept", "*/*");
    
    LOG_DEBUG << "=== Request Body ===";
    LOG_DEBUG << requestBody;

    // 发送请求
    auto [result, response] = client->sendRequest(req);
    
    if (result != ReqResult::Ok) {
        LOG_ERROR << "Failed to send request";
        return;
    }
    
    // 获取响应码和响应体
    int statusCode = response->getStatusCode();
    std::string responseBody = std::string(response->getBody());  // 显式转换
    
    LOG_DEBUG << "=== Response ===";
    LOG_DEBUG << "Status Code: " << statusCode;
    LOG_DEBUG << "=== Response Body ===";
    LOG_DEBUG << responseBody;
    
    // 解析响应
    Json::Value resp_json;
    Json::Reader reader;
    if(reader.parse(responseBody, resp_json)) {
        if (resp_json.isMember("message")) {
            creationTime = resp_json["message"]["creationTime"].asString();
            LOG_INFO << "creationTime: " << creationTime;
        }
    } else {
        LOG_ERROR << "Failed to parse response JSON";
    }
    
    if (creationTime.empty()) {
        LOG_ERROR << "Failed to send message";
    }
}

void Chaynsapi::getMessage(shared_ptr<Accountinfo_st> accountinfo,string threadid,string usermessageid,string& creationTime,string& response_message,int& response_statusCode)
{
    LOG_INFO << "Chaynsapi::getMessage";
     // 创建HTTP客户端
        auto client = HttpClient::newHttpClient("https://intercom.tobit.cloud");
        auto req = HttpRequest::newHttpRequest();
        
        // 构建请求路径
    std::string path;
    path.reserve(100); // 预分配合适的空间
    path.append("/api/thread/").append(threadid)
        .append("/messages?memberId=").append(usermessageid)
        .append("&date=").append(creationTime);
    
    req->setMethod(HttpMethod::Get);
    req->setPath(path);
    req->setContentTypeString("application/json");
    req->addHeader("Authorization", accountinfo->authToken);
    req->addHeader("Accept", "*/*");
    
    LOG_INFO << "url: " << path;

    // 使用指数退避的重试策略
    for(int retry = 0; retry < MAX_RETRIES; retry++) {
        if(retry > 0) {
            int delay = std::max((BASE_DELAY+BASE_DELAY) / (1 << retry), 100); // 最少延迟100ms
            LOG_INFO << "Retry attempt " << retry << " after " << delay << "ms";
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }

        auto [result, response] = client->sendRequest(req);
        
        if (result != ReqResult::Ok) {
            LOG_ERROR << "Failed to send request";
            continue;
        }

        int statusCode = response->getStatusCode();
        
        // 对于204状态码快速处理
        if(statusCode == 204) {
            LOG_INFO << "Message not ready yet, will retry...";
            continue;
        }

        // 使用string_view避免不必要的拷贝
        std::string_view responseBody(response->getBody());
        
        Json::Value resp_json;
        Json::Reader reader;
        if(reader.parse(responseBody.data(), responseBody.data() + responseBody.size(), resp_json)) {
            const auto& messages = resp_json["messages"];
            if (!messages.empty()) {
                const auto& firstMessage = messages[0];
                response_message = firstMessage["text"].asString();
                creationTime = firstMessage["creationTime"].asString();
                response_statusCode=200;
                LOG_INFO << "Successfully retrieved message";
                return;
            }
        } else {
            LOG_ERROR << "Failed to parse response JSON";
        }
    }
    
    LOG_ERROR << "Failed to get message after " << MAX_RETRIES << " attempts";
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
    if(chatinfoMap.find(session.preConversationId)!=chatinfoMap.end())
    {
        LOG_INFO << "find chatinfo in chatinfoMap,recoverChatinfoResponse by ConversationId: " << session.preConversationId;
        std::lock_guard<std::mutex> lock(chatinfoMap_mutex);
        chatinfo_st chatinfo=chatinfoMap[session.preConversationId];
        chatinfo.status=2;
        chatinfoMap[session.curConversationId]=chatinfo;
        chatinfoMap.erase(session.preConversationId);
    }
    else
    {
        LOG_ERROR << "not find chatinfo in chatinfoMap,recoverChatinfoResponse failed by  ConversationId: " << session.preConversationId;
    }
   
}
void Chaynsapi::eraseChatinfoMap(string ConversationId)
{
    std::lock_guard<std::mutex> lock(chatinfoMap_mutex);
    chatinfoMap.erase(ConversationId);
}
Json::Value Chaynsapi::getModels()
{
   return model_info;
}
void* Chaynsapi::createApi()
{
    Chaynsapi* chaynsapi=new Chaynsapi();
    chaynsapi->init();
    return chaynsapi;
}