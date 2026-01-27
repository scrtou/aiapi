#include "channelDbManager.h"
#include <algorithm>  // for std::transform

// PostgreSQL create table SQL
std::string channelTableName = "channel";
std::string createChannelTablePgSql = R"(
    CREATE TABLE IF NOT EXISTS channel (
        id SERIAL PRIMARY KEY,
        channelname VARCHAR(255) UNIQUE NOT NULL,
        channeltype VARCHAR(100) NOT NULL,
        channelurl VARCHAR(500),
        channelkey VARCHAR(500),
        channelstatus BOOLEAN DEFAULT true,
        maxconcurrent INT DEFAULT 10,
        timeout INT DEFAULT 30,
        priority INT DEFAULT 0,
        description TEXT,
        createtime TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updatetime TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        accountcount INT DEFAULT 0,
        supports_tool_calls BOOLEAN DEFAULT true
    );
)";

std::string createChannelTableSqlMysql = R"(
    CREATE TABLE IF NOT EXISTS channel (
        id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
        channelname VARCHAR(255) UNIQUE NOT NULL,
        channeltype VARCHAR(100) NOT NULL,
        channelurl VARCHAR(500),
        channelkey VARCHAR(500),
        channelstatus TINYINT(1) DEFAULT 1,
        maxconcurrent INT DEFAULT 10,
        timeout INT DEFAULT 30,
        priority INT DEFAULT 0,
        description TEXT,
        createtime DATETIME DEFAULT CURRENT_TIMESTAMP,
        updatetime DATETIME DEFAULT CURRENT_TIMESTAMP,
        accountcount INT DEFAULT 0,
        supports_tool_calls TINYINT(1) DEFAULT 1
    ) ENGINE=InnoDB;
)";

std::string createChannelTableSqlite3 = R"(
    CREATE TABLE IF NOT EXISTS channel (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        channelname TEXT UNIQUE NOT NULL,
        channeltype TEXT NOT NULL,
        channelurl TEXT,
        channelkey TEXT,
        channelstatus INTEGER DEFAULT 1,
        maxconcurrent INTEGER DEFAULT 10,
        timeout INTEGER DEFAULT 30,
        priority INTEGER DEFAULT 0,
        description TEXT,
        createtime DATETIME DEFAULT CURRENT_TIMESTAMP,
        updatetime DATETIME DEFAULT CURRENT_TIMESTAMP,
        accountcount INTEGER DEFAULT 0,
        supports_tool_calls INTEGER DEFAULT 1
    );
)";

void ChannelDbManager::detectDbType()
{
    // 从配置文件的 custom_config 读取数据库类型
    auto customConfig = drogon::app().getCustomConfig();
    std::string dbTypeStr = "postgresql";  // 默认值
    
    if (customConfig.isMember("dbtype")) {
        dbTypeStr = customConfig["dbtype"].asString();
    }
    
    // 转换为小写进行比较
    std::transform(dbTypeStr.begin(), dbTypeStr.end(), dbTypeStr.begin(), ::tolower);
    
    if (dbTypeStr == "sqlite3" || dbTypeStr == "sqlite") {
        dbType = DbType::SQLite3;
        LOG_INFO << "[渠道数据库] 配置的数据库类型: SQLite3";
    } else if (dbTypeStr == "mysql" || dbTypeStr == "mariadb") {
        dbType = DbType::MySQL;
        LOG_INFO << "[渠道数据库] 配置的数据库类型: MySQL";
    } else {
        dbType = DbType::PostgreSQL;
        LOG_INFO << "[渠道数据库] 配置的数据库类型: PostgreSQL";
    }
}

void ChannelDbManager::init()
{
    LOG_INFO << "[渠道数据库] 初始化开始";
    detectDbType();
    if (!isTableExist())
    {
        LOG_INFO << "[渠道数据库] 渠道表不存在, 正在创建表...";
        createTable();
    }
    LOG_INFO << "[渠道数据库] 初始化完成";
}

bool ChannelDbManager::addChannel(struct Channelinfo_st channelinfo)
{
    std::string selectsql = "select * from channel where channelname=$1";
    std::string insertsql = "insert into channel (channelname, channeltype, channelurl, channelkey, channelstatus, maxconcurrent, timeout, priority, description, accountcount, supports_tool_calls) values ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)";
    
    auto result = dbClient->execSqlSync(selectsql, channelinfo.channelName);
    if(result.size() != 0)
    {
        LOG_INFO << "渠道 " << channelinfo.channelName << " 已存在，更新渠道";
        return updateChannel(channelinfo);
    }
    else
    {
        auto result1 = dbClient->execSqlSync(insertsql, channelinfo.channelName,
                                             channelinfo.channelType, channelinfo.channelUrl,
                                             channelinfo.channelKey, channelinfo.channelStatus,
                                             channelinfo.maxConcurrent, channelinfo.timeout,
                                             channelinfo.priority, channelinfo.description,
                                             channelinfo.accountCount, channelinfo.supportsToolCalls);
        if(result1.affectedRows() != 0)
        {
            LOG_INFO << "渠道 " << channelinfo.channelName << " 添加成功";
            return true;
        }
        else
        {
            LOG_ERROR << "渠道 " << channelinfo.channelName << " 添加失败";
            return false;
        }
    }
}

bool ChannelDbManager::updateChannel(struct Channelinfo_st channelinfo)
{
    std::string updatesql = "update channel set channeltype=$1, channelurl=$2, channelkey=$3, channelstatus=$4, maxconcurrent=$5, timeout=$6, priority=$7, description=$8, accountcount=$9, supports_tool_calls=$10, updatetime=CURRENT_TIMESTAMP where channelname=$11";
    auto result = dbClient->execSqlSync(updatesql, channelinfo.channelType,
                                        channelinfo.channelUrl, channelinfo.channelKey,
                                        channelinfo.channelStatus, channelinfo.maxConcurrent,
                                        channelinfo.timeout, channelinfo.priority,
                                        channelinfo.description, channelinfo.accountCount,
                                        channelinfo.supportsToolCalls, channelinfo.channelName);
    if(result.affectedRows() != 0)
    {
        LOG_INFO << "渠道 " << channelinfo.channelName << " 更新成功";
        return true;
    }
    else
    {
        LOG_ERROR << "渠道 " << channelinfo.channelName << " 更新失败";
        return false;
    }
}

bool ChannelDbManager::deleteChannel(int channelId)
{
    std::string deletesql = "delete from channel where id=$1";
    auto result = dbClient->execSqlSync(deletesql, channelId);
    if(result.affectedRows() != 0)
    {
        LOG_INFO << "渠道ID " << channelId << " 删除成功";
        return true;
    }
    else
    {
        LOG_ERROR << "渠道ID " << channelId << " 删除失败";
        return false;
    }
}

bool ChannelDbManager::getChannel(string channelName, struct Channelinfo_st& channelinfo)
{
    std::string selectsql;
    if (dbType == DbType::SQLite3) {
        // SQLite3: use datetime() instead of to_char()
        selectsql = "select id, channelname, channeltype, channelurl, channelkey, channelstatus, maxconcurrent, timeout, priority, description, datetime(createtime) as createtime, datetime(updatetime) as updatetime, COALESCE(accountcount, 0) as accountcount, COALESCE(supports_tool_calls, 1) as supports_tool_calls from channel where channelname=$1";
    } else {
        // PostgreSQL: use to_char() for date formatting
        selectsql = "select id, channelname, channeltype, channelurl, channelkey, channelstatus, maxconcurrent, timeout, priority, description, to_char(createtime, 'YYYY-MM-DD HH24:MI:SS') as createtime, to_char(updatetime, 'YYYY-MM-DD HH24:MI:SS') as updatetime, COALESCE(accountcount, 0) as accountcount, COALESCE(supports_tool_calls, true) as supports_tool_calls from channel where channelname=$1";
    }
    auto result = dbClient->execSqlSync(selectsql, channelName);
    if(result.size() != 0)
    {
        const auto& item = result[0];
        channelinfo.id = item["id"].as<int>();
        channelinfo.channelName = item["channelname"].as<std::string>();
        channelinfo.channelType = item["channeltype"].as<std::string>();
        channelinfo.channelUrl = item["channelurl"].as<std::string>();
        channelinfo.channelKey = item["channelkey"].as<std::string>();
        channelinfo.channelStatus = item["channelstatus"].as<bool>();
        channelinfo.maxConcurrent = item["maxconcurrent"].as<int>();
        channelinfo.timeout = item["timeout"].as<int>();
        channelinfo.priority = item["priority"].as<int>();
        channelinfo.description = item["description"].as<std::string>();
        channelinfo.createTime = item["createtime"].as<std::string>();
        channelinfo.updateTime = item["updatetime"].as<std::string>();
        channelinfo.accountCount = item["accountcount"].as<int>();
        channelinfo.supportsToolCalls = item["supports_tool_calls"].as<bool>();
        return true;
    }
    else
    {
        LOG_WARN << "渠道 " << channelName << " 不存在";
        return false;
    }
}

list<Channelinfo_st> ChannelDbManager::getChannelList()
{
    std::string selectsql;
    if (dbType == DbType::SQLite3) {
        // SQLite3: use datetime() instead of to_char()
        selectsql = "select id, channelname, channeltype, channelurl, channelkey, channelstatus, maxconcurrent, timeout, priority, description, datetime(createtime) as createtime, datetime(updatetime) as updatetime, COALESCE(accountcount, 0) as accountcount, COALESCE(supports_tool_calls, 1) as supports_tool_calls from channel order by id";
    } else {
        // PostgreSQL: use to_char() for date formatting
        selectsql = "select id, channelname, channeltype, channelurl, channelkey, channelstatus, maxconcurrent, timeout, priority, description, to_char(createtime, 'YYYY-MM-DD HH24:MI:SS') as createtime, to_char(updatetime, 'YYYY-MM-DD HH24:MI:SS') as updatetime, COALESCE(accountcount, 0) as accountcount, COALESCE(supports_tool_calls, true) as supports_tool_calls from channel order by id";
    }
    auto result = dbClient->execSqlSync(selectsql);
    list<Channelinfo_st> channelList;
    for(auto& item : result)
    {
        Channelinfo_st channelinfo(
            item["id"].as<int>(),
            item["channelname"].as<std::string>(),
            item["channeltype"].as<std::string>(),
            item["channelurl"].as<std::string>(),
            item["channelkey"].as<std::string>(),
            item["channelstatus"].as<bool>(),
            item["maxconcurrent"].as<int>(),
            item["timeout"].as<int>(),
            item["priority"].as<int>(),
            item["description"].as<std::string>(),
            item["createtime"].as<std::string>(),
            item["updatetime"].as<std::string>(),
            item["accountcount"].as<int>(),
            item["supports_tool_calls"].as<bool>()
        );
        channelList.push_back(channelinfo);
    }
    return channelList;
}

bool ChannelDbManager::updateChannelStatus(string channelName, bool status)
{
    std::string updatesql = "update channel set channelstatus=$1, updatetime=CURRENT_TIMESTAMP where channelname=$2";
    auto result = dbClient->execSqlSync(updatesql, status, channelName);
    if(result.affectedRows() != 0)
    {
        LOG_INFO << "渠道 " << channelName << " 状态更新成功";
        return true;
    }
    else
    {
        LOG_ERROR << "渠道 " << channelName << " 状态更新失败";
        return false;
    }
}

bool ChannelDbManager::isTableExist()
{
    if (dbType == DbType::SQLite3)
    {
        // SQLite3: 使用 sqlite_master
        auto result = dbClient->execSqlSync("SELECT name FROM sqlite_master WHERE type='table' AND name='" + channelTableName + "'");
        return result.size() != 0;
    }
    else
    {
        // PostgreSQL/MySQL: 使用 information_schema
        auto result = dbClient->execSqlSync("SELECT table_name FROM information_schema.tables WHERE table_name='" + channelTableName + "'");
        return result.size() != 0;
    }
}

void ChannelDbManager::createTable()
{
    try
    {
        if (dbType == DbType::SQLite3) {
            dbClient->execSqlSync(createChannelTableSqlite3);
        } else if (dbType == DbType::MySQL) {
            dbClient->execSqlSync(createChannelTableSqlMysql);
        } else {
            dbClient->execSqlSync(createChannelTablePgSql);
        }
        LOG_INFO << "[渠道数据库] 渠道表创建成功";
    }
    catch(const std::exception& e)
    {
        LOG_ERROR << "[渠道数据库] 创建表错误: " << e.what();
    }
}

void ChannelDbManager::checkAndUpgradeTable()
{
    bool hasAccountCount = false;
    bool hasSupportsToolCalls = false;
    
    if (dbType == DbType::SQLite3)
    {
        // SQLite3: 使用 PRAGMA table_info
        std::string checkSql = "PRAGMA table_info(channel)";
        auto result = dbClient->execSqlSync(checkSql);
        
        for (const auto& row : result)
        {
            std::string colName = row["name"].as<std::string>();
            if (colName == "accountcount")
            {
                hasAccountCount = true;
            }
            if (colName == "supports_tool_calls")
            {
                hasSupportsToolCalls = true;
            }
        }
    }
    else
    {
        // PostgreSQL/MySQL: 使用 information_schema
        auto result1 = dbClient->execSqlSync("SELECT column_name FROM information_schema.columns WHERE table_name='channel' AND column_name='accountcount'");
        hasAccountCount = (result1.size() > 0);
        
        auto result2 = dbClient->execSqlSync("SELECT column_name FROM information_schema.columns WHERE table_name='channel' AND column_name='supports_tool_calls'");
        hasSupportsToolCalls = (result2.size() > 0);
    }
    
    if (!hasAccountCount)
    {
        LOG_INFO << "[渠道数据库] 表'channel'中缺少列'accountcount', 正在添加...";
        try {
            if (dbType == DbType::SQLite3) {
                dbClient->execSqlSync("ALTER TABLE channel ADD COLUMN accountcount INTEGER DEFAULT 0");
            } else {
                dbClient->execSqlSync("ALTER TABLE channel ADD COLUMN accountcount INT DEFAULT 0");
            }
            LOG_INFO << "[渠道数据库] 列'accountcount'添加成功";
        } catch(const std::exception& e) {
            LOG_ERROR << "[渠道数据库] 添加列'accountcount'失败: " << e.what();
        }
    }
    
    if (!hasSupportsToolCalls)
    {
        LOG_INFO << "[渠道数据库] 表'channel'中缺少列'supports_tool_calls', 正在添加...";
        try {
            if (dbType == DbType::SQLite3) {
                dbClient->execSqlSync("ALTER TABLE channel ADD COLUMN supports_tool_calls INTEGER DEFAULT 1");
            } else {
                dbClient->execSqlSync("ALTER TABLE channel ADD COLUMN supports_tool_calls BOOLEAN DEFAULT true");
            }
            LOG_INFO << "[渠道数据库] 列'supports_tool_calls'添加成功";
        } catch(const std::exception& e) {
            LOG_ERROR << "[渠道数据库] 添加列'supports_tool_calls'失败: " << e.what();
        }
    }
}