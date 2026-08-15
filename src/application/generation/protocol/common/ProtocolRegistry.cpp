#include <application/generation/protocol/common/ProtocolRegistry.h>
#include <application/generation/protocol/claude/ClaudeProtocolModule.h>
#include <application/generation/protocol/openai/OpenAiProtocolModule.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>

namespace generation::protocol {

std::string ProtocolRegistry::normalizeMethod(std::string method)
{
    std::transform(method.begin(), method.end(), method.begin(), [](unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });
    return method;
}

std::string ProtocolRegistry::routeKey(const std::string& method, const std::string& path)
{
    return normalizeMethod(method) + " " + path;
}

bool ProtocolRegistry::registerModule(std::shared_ptr<IProtocolModule> module)
{
    lastError_.clear();
    if (!module || module->id().empty()) {
        lastError_ = "protocol module must have a non-empty id";
        return false;
    }
    if (modules_.find(module->id()) != modules_.end()) {
        lastError_ = "duplicate protocol module: " + module->id();
        return false;
    }
    const auto declared = module->operations();
    if (declared.empty()) {
        lastError_ = "protocol module has no operations: " + module->id();
        return false;
    }
    std::map<std::string, bool> unique;
    for (const auto& operation : declared) {
        if (operation.empty() || !unique.emplace(operation, true).second) {
            lastError_ = "duplicate or empty operation in protocol module: " + module->id();
            return false;
        }
        // Force module implementations to provide both boundary objects.
        try {
            (void)module->requestAdapter(operation);
            (void)module->responseSinkFactory(operation);
        } catch (...) {
            lastError_ = "protocol module has incomplete operation wiring: " + operation;
            return false;
        }
    }
    modules_.emplace(module->id(), std::move(module));
    return true;
}

bool ProtocolRegistry::registerRoute(std::string method,
                                     std::string path,
                                     std::string protocolId,
                                     std::string operation)
{
    lastError_.clear();
    if (!find(protocolId)) {
        lastError_ = "route references unknown protocol: " + protocolId;
        return false;
    }
    const auto* module = find(protocolId);
    const auto operations = module->operations();
    if (std::find(operations.begin(), operations.end(), operation) == operations.end()) {
        lastError_ = "route references unknown operation: " + operation;
        return false;
    }
    const auto key = routeKey(method, path);
    if (routes_.find(key) != routes_.end()) {
        lastError_ = "duplicate protocol route: " + key;
        return false;
    }
    routes_.emplace(key, Route{std::move(protocolId), std::move(operation)});
    return true;
}

const IProtocolModule* ProtocolRegistry::find(const std::string& protocolId) const
{
    const auto it = modules_.find(protocolId);
    return it == modules_.end() ? nullptr : it->second.get();
}

const IProtocolModule* ProtocolRegistry::findRoute(const std::string& method,
                                                   const std::string& path) const
{
    const auto it = routes_.find(routeKey(method, path));
    return it == routes_.end() ? nullptr : find(it->second.protocolId);
}

std::string ProtocolRegistry::findRouteOperation(const std::string& method,
                                                 const std::string& path) const
{
    const auto it = routes_.find(routeKey(method, path));
    return it == routes_.end() ? std::string() : it->second.operation;
}

ProtocolDispatchResult ProtocolRegistry::dispatch(const RawProtocolRequest& raw) const
{
    const IProtocolModule* module = findRoute(raw.method, raw.path);
    const std::string operation = findRouteOperation(raw.method, raw.path);
    if (!module) {
        return ProtocolDispatchResult{
            nullptr, nullptr, operation, {},
            AdapterResult{{}, platform::Error::badRequest("Unknown protocol route")}};
    }
    try {
        const auto& sinkFactory = module->responseSinkFactory(operation);
        return ProtocolDispatchResult{
            module,
            &sinkFactory,
            operation,
            module->capabilityMapper().capabilities(operation),
            module->requestAdapter(operation).adapt(raw)};
    } catch (const std::exception& error) {
        return ProtocolDispatchResult{
            module, nullptr, operation, {},
            AdapterResult{{}, platform::Error::badRequest(error.what())}};
    }
}

bool ProtocolRegistry::validate(std::string* error) const
{
    std::string detail;
    if (modules_.empty()) detail = "no protocol modules registered";
    for (const auto& entry : routes_) {
        if (!find(entry.second.protocolId)) {
            detail = "route references missing protocol: " + entry.second.protocolId;
            break;
        }
    }
    if (error) *error = detail;
    return detail.empty();
}

std::shared_ptr<ProtocolRegistry> makeDefaultProtocolRegistry()
{
    auto registry = std::make_shared<ProtocolRegistry>();
    auto openAiModule = openai::makeOpenAiProtocolModule();
    auto claudeModule = claude::makeClaudeProtocolModule();
    if (!registry->registerModule(openAiModule) ||
        !registry->registerModule(claudeModule)) {
        return nullptr;
    }

    const auto registerRoute = [registry](const std::string& path,
                                           const std::string& operation) {
        return registry->registerRoute("POST", path, "openai-compatible", operation);
    };
    if (!registerRoute("/chaynsapi/v1/chat/completions", "chat.completions") ||
        !registerRoute("/retoolapi/v1/chat/completions", "chat.completions") ||
        !registerRoute("/v1/chat/completions", "chat.completions") ||
        !registerRoute("/chaynsapi/v1/responses", "responses.create") ||
        !registerRoute("/retoolapi/v1/responses", "responses.create") ||
        !registerRoute("/v1/responses", "responses.create")) {
        return nullptr;
    }

    const auto registerClaudeRoute = [registry](const std::string& path) {
        return registry->registerRoute(
            "POST", path, "anthropic-messages", "messages.create");
    };
    if (!registerClaudeRoute("/chaynsapi/v1/messages") ||
        !registerClaudeRoute("/retoolapi/v1/messages") ||
        !registerClaudeRoute("/v1/messages")) {
        return nullptr;
    }
    return registry;
}

}  // namespace generation::protocol
