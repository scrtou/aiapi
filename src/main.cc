#include <drogon/drogon.h>
#include<accountManager/accountManager.h>
#include<apiManager/ApiManager.h>
#include <filesystem>
#include <iostream>

int main() {
    // 清理日志目录中的所有文件
    std::string logDirPath = "../logs";
    try {
        if (std::filesystem::exists(logDirPath)) {
            LOG_INFO << "清理日志目录: " << logDirPath;
            for (const auto& entry : std::filesystem::directory_iterator(logDirPath)) {
                if (entry.is_regular_file()) {
                    LOG_INFO << "删除文件: " << entry.path().string();
                    std::filesystem::remove(entry.path());
                }
            }
        } else {
            LOG_INFO << "创建日志目录: " << logDirPath;
            std::filesystem::create_directory(logDirPath);
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "处理日志目录时出错: " << e.what();
    }
    
    //Set HTTP listener address and port
    //drogon::app().addListener("0.0.0.0", 5555);
    //Load config file
    drogon::app().loadConfigFile("../config.json");
    //drogon::app().loadConfigFile("../config.yaml");
    
    
    // 在事件循环开始后立即执行
    app().getLoop()->queueInLoop([](){
        std::thread t1([]{ 
            AccountManager::getInstance().init();
            ApiManager::getInstance().init();
            chatSession::getInstance()->startClearExpiredSession();
        });
        t1.detach();
    });

    drogon::app().run();

    return 0;
}
