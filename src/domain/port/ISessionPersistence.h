#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct SessionPersistenceRow
{
    std::string sessionId;
    std::string apiName;
    int apiType = 0;
    std::string contextKey;
    std::string payloadJson;
    int64_t createdAt = 0;
    int64_t lastActiveAt = 0;
};

struct ResponsePersistenceRow
{
    std::string responseId;
    std::string sessionId;
    std::string responseJson;
    bool hasResponse = false;
    int64_t createdAt = 0;
};

class ISessionPersistence
{
  public:
    virtual ~ISessionPersistence() = default;

    virtual bool ensureTables(std::string* error = nullptr) = 0;
    virtual bool isEnabled() const = 0;
    virtual void setEnabled(bool enabled) = 0;

    virtual std::optional<SessionPersistenceRow> loadSession(
        const std::string& sessionId, std::string* error = nullptr) = 0;
    virtual std::optional<SessionPersistenceRow> loadSessionByContextKey(
        const std::string& contextKey, std::string* error = nullptr) = 0;
    virtual std::optional<ResponsePersistenceRow> loadResponse(
        const std::string& responseId, std::string* error = nullptr) = 0;

    virtual void asyncUpsertSession(const SessionPersistenceRow& row) = 0;
    virtual void asyncUpsertResponse(const ResponsePersistenceRow& row) = 0;
    virtual void asyncDeleteSessions(const std::vector<std::string>& sessionIds) = 0;
    virtual void asyncDeleteResponses(const std::vector<std::string>& responseIds) = 0;
    virtual int deleteSessionsOlderThan(
        int64_t cutoffEpochSeconds, std::string* error = nullptr) = 0;
};
