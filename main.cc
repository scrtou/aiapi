#include <drogon/drogon.h>
#include<accountManager/accountManager.h>
#include<apiManager/ApiManager.h>
int main() {
    //Set HTTP listener address and port
    //drogon::app().addListener("0.0.0.0", 5555);
    //Load config file
    drogon::app().loadConfigFile("../config.json");
    //drogon::app().loadConfigFile("../config.yaml");


    //开启线程执行
    thread t1([]{
        ApiManager::getInstance().init();
        AccountManager::getInstance().init();
        //AccountManager::getInstance().checkUpdateTokenthread();
    });
    t1.detach();
    drogon::app().run();

    return 0;
}
