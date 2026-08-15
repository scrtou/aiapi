#pragma once

#include <application/generation/contracts/GenerationRequest.h>
#include <application/generation/contracts/IResponseSink.h>
#include <domain/model/AiApiData.h>
#include <platform/result/Error.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace generation::protocol {

class IProtocolModule;
class IProtocolResponseSinkFactory;

struct ProtocolDescriptor {
    std::string id;
    std::string version;
    std::vector<std::string> operations;
};

struct ProtocolExtensions {
    Json::Value request{Json::objectValue};
    Json::Value response{Json::objectValue};
};

/** A copied request at the protocol boundary; no Drogon request leaks inward. */
struct RawProtocolRequest {
    std::string method;
    std::string path;
    Json::Value body{Json::objectValue};
    aiapi::RequestHeaders headers;
};

struct AdapterResult {
    std::optional<GenerationRequest> request;
    std::optional<platform::Error> error;

    bool succeeded() const noexcept { return request.has_value() && !error.has_value(); }
};

struct ProtocolDispatchResult {
    const IProtocolModule* module = nullptr;
    const IProtocolResponseSinkFactory* responseSinkFactory = nullptr;
    std::string operation;
    GenerationCapabilities protocolCapabilities;
    AdapterResult adaptation;

    bool succeeded() const noexcept {
        return module != nullptr && responseSinkFactory != nullptr && adaptation.succeeded();
    }
};

class IProtocolRequestAdapter {
  public:
    virtual ~IProtocolRequestAdapter() = default;
    virtual AdapterResult adapt(const RawProtocolRequest& raw) const = 0;
};

/** Transport callbacks used by a protocol-owned sink factory. */
struct ResponseContext {
    std::string operation;
    std::string model;
    bool stream = false;
    bool nativeResponsesToolItems = false;
    int inputTokensEstimated = 0;
    std::function<void(const Json::Value&, int)> jsonResponse;
    std::function<bool(const std::string&)> streamWriter;
    std::function<void()> close;
};

class IProtocolResponseSinkFactory {
  public:
    virtual ~IProtocolResponseSinkFactory() = default;
    virtual std::shared_ptr<IResponseSink> create(const ResponseContext& context) const = 0;
};

class ICapabilityMapper {
  public:
    virtual ~ICapabilityMapper() = default;
    virtual GenerationCapabilities capabilities(const std::string& operation) const = 0;
};

class IProtocolModule {
  public:
    virtual ~IProtocolModule() = default;
    virtual std::string id() const = 0;
    virtual std::string version() const = 0;
    virtual std::vector<std::string> operations() const = 0;
    virtual ProtocolDescriptor descriptor() const
    {
        return ProtocolDescriptor{id(), version(), operations()};
    }
    virtual const IProtocolRequestAdapter& requestAdapter(
        const std::string& operation) const = 0;
    virtual const IProtocolResponseSinkFactory& responseSinkFactory(
        const std::string& operation) const = 0;
    virtual const ICapabilityMapper& capabilityMapper() const = 0;
};

}  // namespace generation::protocol
