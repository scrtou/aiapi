#pragma once

#include <chrono>
#include <string>

class IResponseIndex
{
  public:
    virtual ~IResponseIndex() = default;

    virtual bool tryGetSessionId(const std::string& responseId,
                                 std::string& outSessionId) = 0;
    virtual void bind(const std::string& responseId,
                      const std::string& sessionId) = 0;
    virtual bool tryGetResponse(const std::string& responseId,
                                std::string& outResponseJson) = 0;
    virtual void storeResponse(const std::string& responseId,
                               const std::string& responseJson) = 0;
    virtual bool erase(const std::string& responseId) = 0;
    virtual void cleanup(size_t maxEntries, std::chrono::seconds maxAge) = 0;
};
