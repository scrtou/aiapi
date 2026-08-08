#ifndef DOMAIN_PORT_IACCOUNT_STORE_H
#define DOMAIN_PORT_IACCOUNT_STORE_H

#include <domain/model/AccountData.h>
#include <list>
#include <string>

// Account 持久化端口（R4 依赖倒置试点 C）。
// 上层 accountManager 只依赖本接口，不再依赖 AccountDbManager 具体实现。
// 方法签名逐字取自 AccountDbManager，因此既有调用表达式无需改写。
//
// 成员取舍依据步骤 67.1 的实测调用统计（accountManager 真实调用了哪些）：
//   纳入的 13 个方法均有调用点；
//   init/getAccount/saveAccount/saveAccountList/updateAccountStatus/
//   getAccountStatusById 从未被 accountManager 调用，故不纳入；
//   getDbType() 返回 DbType —— 那是 dbManager 的内部实现细节，
//   纳入会把 dbManager/DbType.h 拖进 domain，被第三道门禁（rc=3）拦下。
//
// countAccountsByChannel 的默认参数只在此处声明一次。
// 虚函数的默认参数按静态类型绑定，若实现类重复声明默认值，
// 经由不同静态类型调用会得到不同结果 —— 属实现类必须遵守的约束。
class IAccountStore
{
  public:
    virtual ~IAccountStore() = default;

    virtual bool addAccount(struct Accountinfo_st accountinfo) = 0;
    virtual bool updateAccount(struct Accountinfo_st accountinfo) = 0;
    virtual bool deleteAccount(std::string apiName, std::string userName) = 0;
    virtual bool isTableExist() = 0;
    virtual void createTable() = 0;
    virtual void checkAndUpgradeTable() = 0;
    virtual std::list<Accountinfo_st> getAccountDBList() = 0;

    // 状态预占相关
    virtual int createWaitingAccount(std::string apiName) = 0;
    virtual bool activateAccount(int waitingId, struct Accountinfo_st accountinfo) = 0;
    virtual bool deleteWaitingAccount(int waitingId) = 0;
    virtual int countAccountsByChannel(std::string apiName, bool includeWaiting = true) = 0;
    virtual bool updateAccountStatusById(int id, std::string status) = 0;
    virtual std::string getAccountStatusByUsername(std::string apiName, std::string userName) = 0;
};

#endif
