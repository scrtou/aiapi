#include <drogon/drogon.h>
#include<accountManager/accountManager.h>
#include<apiManager/ApiManager.h>
#include<channelManager/channelManager.h>
#include<sessionManager/Session.h>
int main() {
    //Set HTTP listener address and port
    //drogon::app().addListener("0.0.0.0", 5555);
    //Load config file
    drogon::app().loadConfigFile("../config.json");
    //drogon::app().loadConfigFile("../config.yaml");

    // 读取会话追踪模式配置
    auto customConfig = drogon::app().getCustomConfig();
    if (customConfig.isMember("session_tracking")) {
        std::string mode = customConfig["session_tracking"].get("mode", "hash").asString();
        if (mode == "zerowidth" || mode == "zero_width") {
            chatSession::getInstance()->setTrackingMode(SessionTrackingMode::ZeroWidth);
            LOG_INFO << "会话追踪模式: ZeroWidth (零宽字符嵌入)";
        } else {
            chatSession::getInstance()->setTrackingMode(SessionTrackingMode::Hash);
            LOG_INFO << "会话追踪模式: Hash (内容哈希)";
        }
    } else {
        LOG_INFO << "会话追踪模式: Hash (默认)";
    }

    drogon::app().registerPreRoutingAdvice(
        [](const drogon::HttpRequestPtr &req,
           drogon::AdviceCallback &&callback,
           drogon::AdviceChainCallback &&chainCallback) {
            if (req->method() == drogon::HttpMethod::Options) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->addHeader("Access-Control-Allow-Origin", "*");
                resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS, PATCH");
                resp->addHeader("Access-Control-Allow-Headers", "*");
                resp->setStatusCode(drogon::k204NoContent);
                callback(resp);
            } else {
                chainCallback();
            }
        });

    drogon::app().registerPostHandlingAdvice(
        [](const drogon::HttpRequestPtr &req, const drogon::HttpResponsePtr &resp) {
            resp->addHeader("Access-Control-Allow-Origin", "*");
        });




    // 在事件循环开始后立即执行
    app().getLoop()->queueInLoop([](){
        std::thread t1([]{
            ChannelManager::getInstance().init();
            AccountManager::getInstance().init();
            ApiManager::getInstance().init();
        });
        t1.detach();
    });

    drogon::app().run();

    return 0;
}
