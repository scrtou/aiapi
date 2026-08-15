# 03. 统一领域模型

## 1. 建模原则

统一模型只表达生成过程中的稳定语义，不直接模拟某个协议的请求 JSON。

协议专属字段放入扩展区，但扩展区不能被核心 Pipeline 解释。

## 2. 消息和内容块

建议将消息内容从简单的文本/图片模型扩展为内容块：

```cpp
enum class ContentBlockType {
    Text,
    Image,
    ToolUse,
    ToolResult,
    Thinking
};

struct ContentBlock {
    ContentBlockType type = ContentBlockType::Text;
    std::string text;
    std::string imageUrl;
    std::string mediaType;

    std::string toolCallId;
    std::string toolName;
    Json::Value toolInput;
    std::string toolInputDelta;

    std::string toolResult;
    bool toolResultIsError = false;
};

struct Message {
    MessageRole role = MessageRole::User;
    std::vector<ContentBlock> blocks;
};
```

角色表示消息来源，工具调用和工具结果表示消息内容。这样不同协议可以使用不同的外部排列方式，而不改变核心语义。

### Canonical 约束

当前代码中的 `Message::blocks` 是唯一内部消息表示。`Message` 不保存文本、工具调用
或工具结果的镜像字段；OpenAI Request Adapter 直接构造 blocks，Responses/Chat Sink
在输出边界将统一事件编码为各自 wire shape。`GenerationPipeline`、`ToolCallBridge` 和
连续性逻辑只读取 `ContentBlock`。

## 3. 工具模型

```cpp
struct ToolDefinition {
    std::string name;
    std::string description;
    Json::Value inputSchema;
    ToolDefinitionKind kind;
    std::string originalName;
    std::string namespacePath;
};

enum class ToolChoiceMode {
    Auto,
    None,
    Any,
    Specific
};

struct ToolChoice {
    ToolChoiceMode mode = ToolChoiceMode::Auto;
    std::optional<std::string> toolName;
};
```

工具调用必须使用稳定的 `toolCallId`，不能使用工具名称代替调用 ID。

`GenerationRequest` 只保存 `toolDefinitions` 和 `toolChoiceSpec`。OpenAI 的原始工具树
位于 `protocolExtensions["openai"]["raw_tools"]`，核心不读取它。进入 Session 执行边界
时由强类型定义生成一次工具编码；`toolDefinitionsSource` 是该编码在 Tool Bridge 清空
活动工具列表前的不可变来源，不是第二个请求事实来源。

## 4. GenerationRequest

统一请求建议包含：

- `provider`；
- `model`；
- `systemPrompt`；
- `messages`；
- `tools`；
- `toolChoice`；
- `stream`；
- `maxOutputTokens`；
- `temperature`；
- `topP`；
- `previousResponseId`；
- `clientInfo`；
- `protocolExtensions`。

协议 ID 与 operation 由 `ProtocolRegistry` 在请求边界维护，不进入核心生成请求。

## 5. GenerationEvent

事件只描述生成状态：

```cpp
enum class GenerationEventType {
    GenerationStarted,
    TextDelta,
    ToolCallStarted,
    ToolArgumentsDelta,
    ToolCallFinished,
    GenerationFinished,
    GenerationError
};

struct GenerationEvent {
    GenerationEventType type;
    std::string itemId;
    std::string toolCallId;
    std::string toolName;
    std::string textDelta;
    std::string argumentsDelta;
    std::string finishReason;
    std::optional<int> inputTokens;
    std::optional<int> outputTokens;
    Json::Value metadata;
};
```

不能在 `GenerationEvent` 中加入某个协议的 `choices`、`delta` 或 SSE event name。

## 6. 统一错误

```cpp
struct GenerationError {
    std::string code;
    std::string message;
    bool retryable = false;
    int retryAfterSeconds = 0;
    Json::Value details;
};
```

协议层负责把统一错误映射为外部错误格式。

## 7. 能力模型

能力应分为协议、模型和 Provider 三层，实际可用能力取交集：

```cpp
struct GenerationCapabilities {
    bool text = true;
    bool images = false;
    bool streaming = false;
    bool tools = false;
    bool parallelTools = false;
    bool reasoning = false;
    bool continuity = false;
};
```

实际请求会同时保存 `protocolCapabilities`、`modelCapabilities`、
`providerCapabilities` 和 `effectiveCapabilities`。模型目录已声明的图片、思考能力
通过非阻塞缓存查询进入模型层，准入阶段不会刷新远端目录；Provider 的原生并行
工具能力进入 Provider 层，应用 Tool Bridge 仍可提供非原生工具语义。若图片、流式
或工具能力在交集后不存在，请求返回
`unsupported_capability`；并行工具则显式串行化并记录 `capabilityDegradations`。

能力未知时不伪造“已声明”：`modelCapabilitiesDeclared=false`，未声明模型使用明确
默认值，同时让观测和后续模型目录补全有明确标记。
