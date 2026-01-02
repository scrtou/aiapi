#include <drogon/drogon.h>
#include<accountManager/accountManager.h>
#include<apiManager/ApiManager.h>
#include<channelManager/channelManager.h>
int main() {
    //Set HTTP listener address and port
    //drogon::app().addListener("0.0.0.0", 5555);
    //Load config file
    drogon::app().loadConfigFile("../config.json");
    //drogon::app().loadConfigFile("../config.yaml");

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
