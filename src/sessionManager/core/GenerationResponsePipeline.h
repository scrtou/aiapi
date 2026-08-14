#ifndef AIAPI_GENERATION_RESPONSE_PIPELINE_H
#define AIAPI_GENERATION_RESPONSE_PIPELINE_H

#include <sessionManager/contracts/GenerationEvent.h>
#include <sessionManager/contracts/IResponseSink.h>
#include <sessionManager/contracts/LegacySessionData.h>
#include <platform/result/Error.h>

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
                               IChannelCatalog* channelCatalog) noexcept;

    void emit(const session_st& session, IResponseSink& sink) const;
    void emitError(ErrorCode code, const std::string& message,
                   IResponseSink& sink) const;
    void emitError(const platform::Error& error, IResponseSink& sink) const;

    bool channelSupportsToolCalls(const std::string& channelName) const;

private:
    chatSession* sessionStore_ = nullptr;
    IChannelCatalog* channelCatalog_ = nullptr;
};

} // namespace generation

#endif
