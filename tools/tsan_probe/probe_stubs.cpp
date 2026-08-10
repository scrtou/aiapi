// TSan 窄范围探针专用桩。仅供 tools/tsan_probe 使用，不参与主构建与 313 用例。
// 签名逐字对齐各自头文件，任何不匹配都会在链接期暴露为 undefined reference。
#include <apiManager/ApiManager.h>
#include <apipoint/chaynsapi/chaynsapi.h>
#include <dbManager/chaynsThread/chaynsThreadDbManager.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace probe {
std::atomic<long> loadCalls{0};
std::atomic<long> deleteCalls{0};
std::atomic<long> purgeCalls{0};
std::atomic<long> bumpCalls{0};
}  // namespace probe

// ---- chaynsThreadDbManager ----
std::shared_ptr<chaynsThreadDbManager> chaynsThreadDbManager::getInstance() {
    static std::shared_ptr<chaynsThreadDbManager> inst(new chaynsThreadDbManager());
    return inst;
}

std::vector<chaynsThreadDbManager::ThreadRow>
chaynsThreadDbManager::loadThreadsOlderThan(int64_t, int, std::string* errorMessage) {
    probe::loadCalls.fetch_add(1, std::memory_order_relaxed);
    if (errorMessage) errorMessage->clear();
    return {};
}

bool chaynsThreadDbManager::deleteThread(const std::string&, std::string* errorMessage) {
    probe::deleteCalls.fetch_add(1, std::memory_order_relaxed);
    if (errorMessage) errorMessage->clear();
    return true;
}

int chaynsThreadDbManager::purgeExhaustedThreads(int, std::string* errorMessage) {
    probe::purgeCalls.fetch_add(1, std::memory_order_relaxed);
    if (errorMessage) errorMessage->clear();
    return 0;
}

int chaynsThreadDbManager::bumpDeleteAttempts(const std::string&, std::string* errorMessage) {
    probe::bumpCalls.fetch_add(1, std::memory_order_relaxed);
    if (errorMessage) errorMessage->clear();
    return 1;
}

// ---- ApiManager ----
ApiManager& ApiManager::getInstance() {
    static ApiManager inst;
    return inst;
}

std::shared_ptr<APIinterface> ApiManager::getApiByApiName(std::string) {
    return nullptr;  // 让 reaper 走 "chaynsapi 未注册，跳过本轮上游删除" 分支
}

// ---- chaynsapi ----
bool chaynsapi::deleteUpstreamThread(const std::string&, const std::string&,
                                     const std::string&, const std::string&) {
    return true;
}

// ---- ApiManager 构造/析构（头文件声明为非 default，需给出定义）----
ApiManager::ApiManager() = default;
ApiManager::~ApiManager() = default;

// ---- chaynsapi 完整 vtable ----
// dynamic_pointer_cast<chaynsapi> 需要 typeinfo；typeinfo/vtable 只在
// 「全部虚函数都有定义」时才会被发射，故这里逐个给出空实现。
// 探针里 getApiByApiName 返回 nullptr，这些函数永远不会被调用。
chaynsapi::chaynsapi() = default;
chaynsapi::~chaynsapi() = default;

provider::ProviderResult chaynsapi::generate(session_st&) { return {}; }
void chaynsapi::checkAlivableTokens() {}
void chaynsapi::checkModels() {}
ProviderModelCatalog chaynsapi::getModels() { return {}; }
void chaynsapi::init() {}
void chaynsapi::afterResponseProcess(session_st&) {}
void chaynsapi::eraseChatinfoMap(std::string) {}
void chaynsapi::transferThreadContext(const std::string&, const std::string&) {}
