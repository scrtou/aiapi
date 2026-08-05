#ifndef AIAPI_ACTION_PROTOCOL_COMPILER_H
#define AIAPI_ACTION_PROTOCOL_COMPILER_H

#include <json/json.h>
#include <cstddef>
#include <string>
#include <vector>

namespace actionproto {

enum class WireFormat {
    Auto,
    JsonV3,
    XmlV2
};

enum class ActionKind {
    ToolCall,
    FinalResponse
};

struct ToolAction {
    std::string id;
    std::string name;
    std::string argumentsJson;
};

struct ActionEnvelope {
    int protocolVersion = 1;
    std::string nonce;
    std::vector<ToolAction> toolCalls;
    std::string finalResponse;

    ActionKind kind() const {
        return !toolCalls.empty() ? ActionKind::ToolCall : ActionKind::FinalResponse;
    }
};

struct ClientCapabilities {
    bool requiresActionEveryTurn = false;
    bool supportsFinalText = true;
    bool supportsParallelCalls = false;
    bool supportsCustomTools = false;
    size_t maxToolCalls = 1;
};

ClientCapabilities capabilitiesForClient(const std::string& clientType,
                                          bool parallelToolCalls);

struct CompileOptions {
    std::string expectedSentinel;
    ClientCapabilities capabilities;
    WireFormat wireFormat = WireFormat::Auto;
};

enum class CompileError {
    None,
    MissingSentinel,
    InvalidEnvelope,
    InvalidActionShape,
    InvalidArgumentsJson,
    MultipleActions,
    MissingAction
};

struct CompileDiagnostic {
    CompileError code = CompileError::None;
    std::string message;
    size_t offset = 0;
};

struct CompileResult {
    bool matched = false;
    bool valid = false;
    ActionEnvelope envelope;
    CompileDiagnostic diagnostic;
};

class ActionProtocolCompiler {
public:
    static CompileResult compileResponse(const std::string& input,
                                         const CompileOptions& options);

    // 生成供不支持原生 tool calls 的上游模型使用的 JSON-only action-v3
    // 规则。compileResponse 仍兼容历史 action-v2 XML。
    static std::string buildRouterPolicy(
        const std::string& sentinel,
        const ClientCapabilities& capabilities);
};

} // namespace actionproto

#endif
