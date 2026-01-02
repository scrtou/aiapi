#include "channelManager.h"

ChannelManager::ChannelManager()
{
}

ChannelManager::~ChannelManager()
{
}

void ChannelManager::init()
{
    LOG_INFO << "ChannelManager::init start";
    channelDbManager = ChannelDbManager::getInstance();
    if (!channelDbManager->isTableExist())
    {
        channelDbManager->createTable();
    }
    LOG_INFO << "ChannelManager::init end";
}

bool ChannelManager::addChannel(struct Channelinfo_st channelinfo)
{
    return channelDbManager->addChannel(channelinfo);
}

bool ChannelManager::updateChannel(struct Channelinfo_st channelinfo)
{
    return channelDbManager->updateChannel(channelinfo);
}

bool ChannelManager::deleteChannel(int channelId)
{
    return channelDbManager->deleteChannel(channelId);
}

list<Channelinfo_st> ChannelManager::getChannelList()
{
    return channelDbManager->getChannelList();
}

bool ChannelManager::updateChannelStatus(string channelName, bool status)
{
    return channelDbManager->updateChannelStatus(channelName, status);
}