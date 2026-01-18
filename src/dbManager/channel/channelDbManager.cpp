#include "channelDbManager.h"

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
        accountcount INT DEFAULT 0
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
        accountcount INT DEFAULT 0
    ) ENGINE=InnoDB;
)";

void ChannelDbManager::init()
{
    LOG_INFO << "ChannelDbManager::init start";
    if (!isTableExist())
    {
        LOG_INFO << "Channel table does not exist, creating table...";
        createTable();
    }
    LOG_INFO << "ChannelDbManager::init end";
}

bool ChannelDbManager::addChannel(struct Channelinfo_st channelinfo)
{
    std::string selectsql = "select * from channel where channelname=$1";
    std::string insertsql = "insert into channel (channelname, channeltype, channelurl, channelkey, channelstatus, maxconcurrent, timeout, priority, description, accountcount) values ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)";
    
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
                                             channelinfo.accountCount);
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
    std::string updatesql = "update channel set channeltype=$1, channelurl=$2, channelkey=$3, channelstatus=$4, maxconcurrent=$5, timeout=$6, priority=$7, description=$8, accountcount=$9, updatetime=CURRENT_TIMESTAMP where channelname=$10";
    auto result = dbClient->execSqlSync(updatesql, channelinfo.channelType,
                                        channelinfo.channelUrl, channelinfo.channelKey,
                                        channelinfo.channelStatus, channelinfo.maxConcurrent,
                                        channelinfo.timeout, channelinfo.priority,
                                        channelinfo.description, channelinfo.accountCount,
                                        channelinfo.channelName);
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
    std::string selectsql = "select id, channelname, channeltype, channelurl, channelkey, channelstatus, maxconcurrent, timeout, priority, description, to_char(createtime, 'YYYY-MM-DD HH24:MI:SS') as createtime, to_char(updatetime, 'YYYY-MM-DD HH24:MI:SS') as updatetime, COALESCE(accountcount, 0) as accountcount from channel where channelname=$1";
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
    std::string selectsql = "select id, channelname, channeltype, channelurl, channelkey, channelstatus, maxconcurrent, timeout, priority, description, to_char(createtime, 'YYYY-MM-DD HH24:MI:SS') as createtime, to_char(updatetime, 'YYYY-MM-DD HH24:MI:SS') as updatetime, COALESCE(accountcount, 0) as accountcount from channel order by id";
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
            item["accountcount"].as<int>()
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
    auto result = dbClient->execSqlSync("select * from information_schema.tables where table_name='" + channelTableName + "'");
    if(result.size() != 0)
    {
        return true;
    }
    else
    {
        return false;
    }
}

void ChannelDbManager::createTable()
{
    try 
    {
        dbClient->execSqlSync(createChannelTablePgSql);
        LOG_INFO << "渠道表创建成功";
    }
    catch(const std::exception& e)
    {
        LOG_ERROR << "createTable error: " << e.what();
    }
}

void ChannelDbManager::checkAndUpgradeTable()
{
    // Check if accountcount column exists
    std::string checkSql = "SELECT column_name FROM information_schema.columns WHERE table_name='channel' AND column_name='accountcount'";
    auto result = dbClient->execSqlSync(checkSql);
    if(result.size() == 0)
    {
        LOG_INFO << "Column 'accountcount' missing in table 'channel', adding it...";
        try {
            dbClient->execSqlSync("ALTER TABLE channel ADD COLUMN accountcount INT DEFAULT 0");
            LOG_INFO << "Column 'accountcount' added successfully.";
        } catch(const std::exception& e) {
            LOG_ERROR << "Failed to add column 'accountcount': " << e.what();
        }
    }
}