#include "accountBackupDbManager.h"

#include <drogon/drogon.h>

using namespace drogon;
using namespace drogon::orm;

namespace {

const char* kBackupDbConnInfo = "filename=./data/account_backup.db";
const char* kBackupTableSql = R"(
    CREATE TABLE IF NOT EXISTS account_backup (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        backup_time DATETIME DEFAULT CURRENT_TIMESTAMP,
        backup_reason TEXT,
        apiname TEXT,
        username TEXT,
        password TEXT,
        authtoken TEXT,
        usecount INTEGER,
        tokenstatus INTEGER,
        accountstatus INTEGER,
        usertobitid INTEGER,
        personid TEXT,
        createtime TEXT,
        accounttype TEXT,
        status TEXT
    );
)";

}  // namespace

AccountBackupDbManager::AccountBackupDbManager()
{
    try {
        dbClient_ = DbClient::newSqlite3Client(kBackupDbConnInfo, 1);
        LOG_INFO << "[备份数据库] 已初始化 SQLite 备份库: ./data/account_backup.db";
    } catch (const std::exception& e) {
        LOG_ERROR << "[备份数据库] 初始化失败: " << e.what();
    }
}

bool AccountBackupDbManager::ensureTable()
{
    if (!dbClient_) {
        LOG_ERROR << "[备份数据库] 数据库客户端不可用";
        return false;
    }

    try {
        dbClient_->execSqlSync(kBackupTableSql);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "[备份数据库] 创建备份表失败: " << e.what();
        return false;
    }
}

bool AccountBackupDbManager::backupAccount(const Accountinfo_st& accountinfo,
                                           const std::string& reason)
{
    if (!ensureTable()) {
        return false;
    }

    static const std::string insertSql =
        "insert into account_backup "
        "(backup_reason, apiname, username, password, authtoken, usecount, "
        "tokenstatus, accountstatus, usertobitid, personid, createtime, "
        "accounttype, status) "
        "values ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13)";

    try {
        auto result = dbClient_->execSqlSync(insertSql,
                                             reason,
                                             accountinfo.apiName,
                                             accountinfo.userName,
                                             accountinfo.passwd,
                                             accountinfo.authToken,
                                             accountinfo.useCount,
                                             accountinfo.tokenStatus,
                                             accountinfo.accountStatus,
                                             accountinfo.userTobitId,
                                             accountinfo.personId,
                                             accountinfo.createTime,
                                             accountinfo.accountType,
                                             accountinfo.status);
        if (result.affectedRows() != 0) {
            LOG_WARN << "[备份数据库] 账号已备份: userName=" << accountinfo.userName
                     << ", reason=" << reason;
            return true;
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "[备份数据库] 备份账号失败: userName=" << accountinfo.userName
                  << ", reason=" << reason
                  << ", error=" << e.what();
    }

    return false;
}

std::list<Accountinfo_st> AccountBackupDbManager::getBackupAccountList()
{
    std::list<Accountinfo_st> accountList;
    if (!ensureTable()) {
        return accountList;
    }

    static const std::string selectSql =
        "select apiname, username, password, authtoken, usecount, tokenstatus, "
        "accountstatus, usertobitid, personid, createtime, accounttype, status "
        "from account_backup order by backup_time desc, id desc";

    try {
        auto result = dbClient_->execSqlSync(selectSql);
        for (const auto& item : result) {
            std::string createTimeStr;
            if (!item["createtime"].isNull()) {
                createTimeStr = item["createtime"].as<std::string>();
            }
            std::string accountTypeStr = "free";
            if (!item["accounttype"].isNull()) {
                accountTypeStr = item["accounttype"].as<std::string>();
            }
            std::string statusStr = AccountStatus::ACTIVE;
            if (!item["status"].isNull()) {
                statusStr = item["status"].as<std::string>();
            }
            accountList.emplace_back(item["apiname"].as<std::string>(),
                                     item["username"].as<std::string>(),
                                     item["password"].as<std::string>(),
                                     item["authtoken"].as<std::string>(),
                                     item["usecount"].as<int>(),
                                     item["tokenstatus"].as<bool>(),
                                     item["accountstatus"].as<bool>(),
                                     item["usertobitid"].as<int>(),
                                     item["personid"].as<std::string>(),
                                     createTimeStr,
                                     accountTypeStr,
                                     statusStr);
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "[备份数据库] 读取备份账号列表失败: " << e.what();
    }

    return accountList;
}
