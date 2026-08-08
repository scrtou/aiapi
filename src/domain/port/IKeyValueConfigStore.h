#ifndef IKEY_VALUE_CONFIG_STORE_H
#define IKEY_VALUE_CONFIG_STORE_H

#include <map>
#include <optional>
#include <string>

// 字符串键值配置持久化端口（沿用 IRetoolWorkspaceStore 的依赖倒置做法）。
// 方法签名逐字取自 ConfigDbManager，因此既有调用表达式无需改写。
// 上层只依赖本接口，具体 ConfigDbManager 适配器留在已被棘轮豁免的文件里。
class IKeyValueConfigStore
{
  public:
    virtual ~IKeyValueConfigStore() = default;

    virtual bool ensureTable(std::string* errorMessage = nullptr) = 0;
    virtual std::optional<std::string> getValue(const std::string& key,
                                                std::string* errorMessage = nullptr) = 0;
    virtual bool setValues(const std::map<std::string, std::string>& entries,
                           std::string* errorMessage = nullptr) = 0;
};

#endif
