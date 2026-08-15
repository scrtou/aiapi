#ifndef AIAPI_GENERATION_RESPONSE_PIPELINE_H
#define AIAPI_GENERATION_RESPONSE_PIPELINE_H

#include <application/generation/contracts/GenerationEvent.h>
#include <application/generation/contracts/IResponseSink.h>
#include <application/generation/contracts/GenerationSession.h>
#include <platform/result/Error.h>

#include <json/json.h>

#include <string>
#include <vector>

class chatSession;
class IChannelCatalog;

namespace generation {

// Response-only stages: decode raw provider output, normalize/validate tool
// calls, then emit the protocol event sequence.  It does not call a provider
// or mutate session persistence.
class GenerationResponsePipeline {
public:
    GenerationResponsePipeline(chatSession* sessionStore,
                               IChannelCatalog* channelCatalog,
                               Json::Value runtimeConfig = Json::Value(Json::objectValue)) noexcept;

    void emit(const session_st& session, IResponseSink& sink) const;
    void emitError(platform::ErrorCode code, const std::string& message,
                   IResponseSink& sink) const;
    void emitError(const platform::Error& error, IResponseSink& sink) const;

    bool channelSupportsToolCalls(const std::string& channelName) const;

private:
    chatSession* sessionStore_ = nullptr;
    IChannelCatalog* channelCatalog_ = nullptr;
    Json::Value runtimeConfig_;
};

} // namespace generation

#endif
