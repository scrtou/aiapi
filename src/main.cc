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
