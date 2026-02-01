# aiapi 调用关系图与接口样例

本文档详细描述 aiapi 的模块拆解、调用关系、时序图以及 curl 测试示例。

---

## 1. 模块拆解

### 1.1 HTTP 层

| 模块 | 文件 | 职责 |
|------|------|------|
| AiApi Controller | `controllers/AiApi.h/cc` | HTTP 路由分发，请求预处理 |
| ChatJsonSink | `controllers/sinks/ChatJsonSink.cpp` | Chat 非流式 JSON 输出 |
| ChatSseSink | `controllers/sinks/ChatSseSink.cpp` | Chat 流式 SSE 输出 |
| ResponsesSseSink | `controllers/sinks/ResponsesSseSink.cpp` | Responses API SSE 输出 |

### 1.2 适配层

| 模块 | 文件 | 职责 |
|------|------|------|
| RequestAdapters | `sessionManager/RequestAdapters.h/cpp` | HTTP → GenerationRequest 转换 |

### 1.3 生成编排层

| 模块 | 文件 | 职责 |
|------|------|------|
| GenerationService | `sessionManager/GenerationService.h/cpp` | 核心编排服务 |
| GenerationRequest | `sessionManager/GenerationRequest.h` | 统一请求结构 |
| GenerationEvent | `sessionManager/GenerationEvent.h` | 统一事件模型 |
| IResponseSink | `sessionManager/IResponseSink.h` | 输出通道接口 |
| Session | `sessionManager/Session.h/cpp` | 会话管理 |
| SessionExecutionGate | `sessionManager/SessionExecutionGate.h` | 并发门控 |
| ToolCallBridge | `sessionManager/ToolCallBridge.h/cpp` | 工具调用桥接 |
| XmlTagToolCallCodec | `sessionManager/XmlTagToolCallCodec.h/cpp` | XML 编解码 |
| ToolCallValidator | `sessionManager/ToolCallValidator.h/cpp` | 工具调用验证 |
| ClientOutputSanitizer | `sessionManager/ClientOutputSanitizer.h/cpp` | 输出清洗 |
| Errors | `sessionManager/Errors.h` | 统一错误模型 |

### 1.4 Provider 层

| 模块 | 文件 | 职责 |
|------|------|------|
| APIinterface | `apipoint/APIinterface.h` | Provider 抽象接口 |
| ProviderResult | `apipoint/ProviderResult.h` | Provider 结果结构 |
| ApiManager | `apiManager/ApiManager.h/cpp` | Provider 路由选择 |
| ApiFactory | `apiManager/ApiFactory.h/cpp` | Provider 工厂 |

---

## 2. 核心数据结构

### 2.1 GenerationRequest（统一请求结构）

```cpp
struct Message {
    std::string role;      // "system", "user", "assistant"
    std::string content;
};

struct ImageInfo {
    std::string url;       // 图片 URL 或 base64 data URL
    std::string mimeType;  // "image/png", "image/jpeg" 等
    std::string base64;    // base64 编码数据（如果是 data URL）
};

struct GenerationRequest {
    std::string model;                      // 模型名称
    std::string systemPrompt;               // 系统提示词
    std::string currentInput;               // 当前用户输入
    std::vector<Message> messages;          // 历史消息
    std::vector<ImageInfo> images;          // 图片信息
    
    bool stream = false;                    // 是否流式输出
    std::optional<int> maxTokens;           // 最大 token 数
    std::optional<double> temperature;      // 温度参数
    
    Json::Value tools;                      // 工具定义
    Json::Value toolChoice;                 // 工具选择策略
    
    Json::Value clientInfo;                 // 客户端信息
    std::string extractedSessionId;         // 从零宽字符提取的 session ID
    std::string previousResponseId;         // 前一个响应 ID（Responses API）
};
```

### 2.2 GenerationEvent（统一事件模型）

```cpp
namespace generation {

// 生成开始事件
struct Started {
    std::string responseId;      // 响应 ID
    std::string model;           // 使用的模型
};

// 文本增量事件（流式）
struct OutputTextDelta {
    std::string delta;           // 增量文本
    int index;                   // 输出项索引
    std::optional<int> outputItemIndex;  // 输出项索引（可选）
};

// 文本完成事件
struct OutputTextDone {
    std::string text;            // 完整文本
    int index;                   // 输出项索引
};

// 工具调用完成事件
struct ToolCallDone {
    std::string id;              // 调用 ID
    std::string name;            // 工具名称
    std::string arguments;       // 参数 JSON 字符串
    int index;                   // 工具调用索引
};

// Token 使用量事件
struct Usage {
    int inputTokens;             // 输入 token 数
    int outputTokens;            // 输出 token 数
};

// 生成完成事件
struct Completed {
    std::string finishReason;    // 完成原因: "stop", "tool_calls", "length"
    std::optional<Usage> usage;  // 使用量统计
};

// 错误事件
struct Error {
    std::string code;            // 错误码
    std::string message;         // 错误消息
    std::string detail;          // 详细信息
};

using GenerationEvent = std::variant<
    Started,
    OutputTextDelta,
    OutputTextDone,
    ToolCallDone,
    Usage,
    Completed,
    Error
>;

} // namespace generation
```

### 2.3 Session（会话结构）

```cpp
// 会话追踪模式
enum class TrackingMode {
    Hash,       // SHA256 哈希追踪（默认）
    ZeroWidth   // 零宽字符嵌入追踪
};

// 会话结构
struct session_st {
    std::string sessionId;           // 会话 ID
    std::string systemPrompt;        // 系统提示词
    std::string systemPromptHash;    // 系统提示词哈希
    std::string contextHash;         // 上下文哈希
    TrackingMode trackingMode;       // 追踪模式
    bool useTextBridge;              // 是否使用文本桥接
    std::chrono::steady_clock::time_point lastActivityTime;  // 最后活动时间
    std::vector<Message> conversationHistory;  // 会话历史
};
```

### 2.4 IResponseSink（输出通道接口）

```cpp
class IResponseSink {
public:
    virtual ~IResponseSink() = default;
    
    // 处理生成事件
    virtual void onEvent(const generation::GenerationEvent& event) = 0;
    
    // 关闭输出通道
    virtual void onClose() = 0;
    
    // 获取 Sink 类型标识
    virtual std::string getSinkType() const = 0;
};
```

### 2.5 Errors（统一错误模型）

```cpp
namespace error {

enum class ErrorCode {
    None = 0,
    BadRequest,      // 400
    Unauthorized,    // 401
    Forbidden,       // 403
    NotFound,        // 404
    Conflict,        // 409 - 并发冲突
    RateLimited,     // 429
    Timeout,         // 504
    ProviderError,   // 502
    Internal,        // 500
    Cancelled        // 499 - 请求被取消
};

struct AppError {
    ErrorCode code = ErrorCode::None;
    std::string message;          // 用户可见消息
    std::string detail;           // 调试详情
    std::string providerCode;     // Provider 原始错误码
    
    bool hasError() const { return code != ErrorCode::None; }
    int httpStatus() const;       // 转换为 HTTP 状态码
    std::string type() const;     // 错误类型字符串
    
    // 工厂方法
    static AppError badRequest(const std::string& msg, const std::string& detail = "");
    static AppError unauthorized(const std::string& msg = "Unauthorized");
    static AppError forbidden(const std::string& msg = "Forbidden");
    static AppError notFound(const std::string& msg = "Not found");
    static AppError conflict(const std::string& msg, const std::string& detail = "");
    static AppError rateLimited(const std::string& msg = "Rate limited");
    static AppError timeout(const std::string& msg = "Request timeout");
    static AppError providerError(const std::string& msg, const std::string& providerCode = "");
    static AppError internal(const std::string& msg, const std::string& detail = "");
    static AppError cancelled(const std::string& msg = "Request cancelled");
};

} // namespace error
```

---

## 3. 工具调用验证（ToolCallValidator）

### 3.1 校验模式

```cpp
namespace toolcall {

// 校验模式
enum class ValidationMode {
    None,      // 不校验 - 信任 AI 输出，只解析 JSON
    Relaxed,   // 宽松校验 - 只校验关键字段（path, content 等必须非空）
    Strict     // 严格校验 - 完整 schema 校验（所有 required + 类型检查）
};

// 校验结果
struct ValidationResult {
    bool valid = false;
    std::string errorMessage;
    
    static ValidationResult success() { return {true, ""}; }
    static ValidationResult failure(const std::string& msg) { return {false, msg}; }
};

} // namespace toolcall
```

### 3.2 ToolCallValidator 类

```cpp
namespace toolcall {

class ToolCallValidator {
public:
    // 构造时传入工具定义和客户端类型
    explicit ToolCallValidator(
        const Json::Value& toolDefs, 
        const std::string& clientType = ""
    );
    
    // 校验单个工具调用
    ValidationResult validate(
        const generation::ToolCallDone& toolCall,
        ValidationMode mode = ValidationMode::None
    ) const;
    
    // 过滤无效的工具调用
    size_t filterInvalidToolCalls(
        std::vector<generation::ToolCallDone>& toolCalls,
        std::string& discardedText,
        ValidationMode mode = ValidationMode::None
    ) const;
    
    // 检查工具名是否存在
    bool hasToolDefinition(const std::string& toolName) const;
    
    // 获取有效工具名集合
    const std::unordered_set<std::string>& getValidToolNames() const;
    
    // 获取客户端类型
    const std::string& getClientType() const;
};

// 降级策略
enum class FallbackStrategy {
    DiscardOnly,           // 仅丢弃无效工具调用
    WrapAttemptCompletion, // 包装为 attempt_completion
    GenerateReadFile       // 生成 read_file 降级调用
};

// 应用降级策略
FallbackStrategy applyValidationFallback(
    const std::string& clientType,
    std::vector<generation::ToolCallDone>& toolCalls,
    std::string& textContent,
    const std::string& discardedText
);

// 检查是否为严格工具客户端
bool isStrictToolClient(const std::string& clientType);

// 获取推荐的校验模式
ValidationMode getRecommendedValidationMode(const std::string& clientType);

} // namespace toolcall
```

---

## 4. 并发门控（SessionExecutionGate）

### 4.1 并发策略

```cpp
namespace session {

enum class ConcurrencyPolicy {
    RejectConcurrent,   // 拒绝并发请求（返回 409 Conflict）
    CancelPrevious      // 取消之前的请求，执行新请求
};

enum class GateResult {
    Acquired,           // 成功获取执行权
    Rejected,           // 被拒绝（已有请求在执行）
    Cancelled           // 之前的请求被取消
};

} // namespace session
```

### 4.2 CancellationToken（取消令牌）

```cpp
namespace session {

class CancellationToken {
public:
    CancellationToken();
    
    void cancel();                // 请求取消
    bool isCancelled() const;     // 检查是否已取消
    void reset();                 // 重置取消状态
};

using CancellationTokenPtr = std::shared_ptr<CancellationToken>;

} // namespace session
```

### 4.3 SessionExecutionGate（执行门控）

```cpp
namespace session {

class SessionExecutionGate {
public:
    static SessionExecutionGate& getInstance();  // 单例
    
    // 尝试获取执行权
    GateResult tryAcquire(
        const std::string& sessionKey,
        ConcurrencyPolicy policy,
        CancellationTokenPtr& outToken
    );
    
    // 释放执行权
    void release(const std::string& sessionKey);
    
    // 检查是否正在执行
    bool isExecuting(const std::string& sessionKey) const;
    
    // 清理过期槽位
    void cleanup(size_t maxIdleSlots = 1000);
};

} // namespace session
```

### 4.4 ExecutionGuard（RAII 守卫）

```cpp
namespace session {

class ExecutionGuard {
public:
    ExecutionGuard(
        const std::string& sessionKey,
        ConcurrencyPolicy policy = ConcurrencyPolicy::RejectConcurrent
    );
    ~ExecutionGuard();  // 自动释放
    
    bool isAcquired() const;              // 是否成功获取
    GateResult getResult() const;         // 获取门控结果
    CancellationTokenPtr getToken() const; // 获取取消令牌
    bool isCancelled() const;             // 检查是否已取消
};

} // namespace session
```

---

## 5. RequestAdapters（请求适配器）

```cpp
class RequestAdapters {
public:
    // 从 Chat Completions API 构建请求
    static GenerationRequest buildGenerationRequestFromChat(
        const drogon::HttpRequestPtr& req
    );
    
    // 从 Responses API 构建请求
    static GenerationRequest buildGenerationRequestFromResponses(
        const drogon::HttpRequestPtr& req
    );
    
private:
    // 提取客户端信息
    static Json::Value extractClientInfo(const drogon::HttpRequestPtr& req);
    
    // 解析 Chat API messages
    static void parseChatMessages(
        const Json::Value& messages,
        std::vector<Message>& result,
        std::string& systemPrompt,
        std::string& currentInput,
        std::vector<ImageInfo>& images,
        std::string& extractedSessionId
    );
    
    // 解析 Responses API input
    static void parseResponseInput(
        const Json::Value& input,
        std::vector<Message>& messages,
        std::string& currentInput,
        std::vector<ImageInfo>& images,
        std::string& extractedSessionId
    );
    
    // 提取 content 文本和图片
    static std::string extractContentText(
        const Json::Value& content,
        std::vector<ImageInfo>& images,
        bool stripZeroWidth = false
    );
    
    // 解析图片 URL
    static ImageInfo parseImageUrl(const std::string& url);
};
```

---

## 6. 调用关系图

### 6.1 整体架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                         HTTP 层                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    AiApi Controller                      │    │
│  │  POST /chaynsapi/v1/chat/completions                    │    │
│  │  POST /chaynsapi/v1/responses                           │    │
│  │  GET  /chaynsapi/v1/models                              │    │
│  │  GET  /chaynsapi/v1/responses/{id}                      │    │
│  │  DELETE /chaynsapi/v1/responses/{id}                    │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        适配层                                    │
│  ┌─────────────────┐     ┌──────────────────────────────────┐  │
│  │ RequestAdapters │ ──▶ │      GenerationRequest           │  │
│  │ (Chat/Responses)│     │ (统一请求结构)                    │  │
│  └─────────────────┘     └──────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      生成编排层                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │               GenerationService                          │    │
│  │  - runGuarded()       (主入口，含并发门控)                │    │
│  │  - materializeSession()                                  │    │
│  │  - computeExecutionKey()                                 │    │
│  │  - executeProvider()                                     │    │
│  │  - emitResultEvents()                                    │    │
│  └─────────────────────────────────────────────────────────┘    │
│           │              │              │              │         │
│           ▼              ▼              ▼              ▼         │
│  ┌──────────────┐ ┌────────────┐ ┌──────────────┐ ┌──────────┐  │
│  │ToolCallBridge│ │  Session   │ │ToolCallValid.│ │OutputSan.│  │
│  └──────────────┘ └────────────┘ └──────────────┘ └──────────┘  │
│           │                                                      │
│           ▼                                                      │
│  ┌──────────────────┐                                            │
│  │SessionExecutionGate│ (并发门控)                               │
│  └──────────────────┘                                            │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Provider 层                                 │
│  ┌─────────────┐     ┌─────────────────────────────────────┐   │
│  │  ApiManager │ ──▶ │         APIinterface                │   │
│  │  (路由选择)  │     │  - generate()                       │   │
│  └─────────────┘     │  - ProviderResult                   │   │
│                       └─────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                       输出层                                     │
│  ┌───────────────┐  ┌───────────────┐  ┌───────────────────┐   │
│  │ IResponseSink │  │GenerationEvent│  │   HTTP Response   │   │
│  │  (接口)       │◀─│  (事件模型)    │──▶│  (JSON/SSE)       │   │
│  └───────────────┘  └───────────────┘  └───────────────────┘   │
│        │                                                         │
│        ├── ChatJsonSink (非流式 JSON)                            │
│        ├── ChatSseSink (流式 SSE)                                │
│        └── ResponsesSseSink (Responses API SSE)                  │
└─────────────────────────────────────────────────────────────────┘
```

### 6.2 请求处理时序图

```
Client              AiApi           RequestAdapters    GenerationService    Provider
  │                   │                   │                   │                │
  │ POST /chat/...    │                   │                   │                │
  │──────────────────>│                   │                   │                │
  │                   │                   │                   │                │
  │                   │ buildRequest()    │                   │                │
  │                   │──────────────────>│                   │                │
  │                   │<──────────────────│                   │                │
  │                   │ GenerationRequest │                   │                │
  │                   │                   │                   │                │
  │                   │ create Sink       │                   │                │
  │                   │ (ChatSseSink)     │                   │                │
  │                   │                   │                   │                │
  │                   │ runGuarded(req, sink, policy)         │                │
  │                   │──────────────────────────────────────>│                │
  │                   │                   │                   │                │
  │                   │                   │  tryAcquire()     │                │
  │                   │                   │  (SessionGate)    │                │
  │                   │                   │                   │                │
  │                   │                   │  materializeSession()              │
  │                   │                   │                   │                │
  │                   │                   │  executeProvider()│                │
  │                   │                   │                   │ generate()     │
  │                   │                   │                   │───────────────>│
  │                   │                   │                   │<───────────────│
  │                   │                   │                   │ ProviderResult │
  │                   │                   │                   │                │
  │                   │                   │  emitResultEvents()                │
  │                   │                   │   │                │                │
  │    SSE events     │<──────────────────│───│                │                │
  │<──────────────────│                   │                   │                │
  │                   │                   │                   │                │
  │                   │                   │  release()        │                │
  │                   │                   │  (SessionGate)    │                │
  │                   │                   │                   │                │
```

### 6.3 工具调用处理流程

```
GenerationService     ToolCallBridge     XmlCodec      ToolCallValidator
       │                    │               │                  │
       │ 检查是否需要桥接    │               │                  │
       │───────────────────>│               │                  │
       │                    │               │                  │
       │ [TextBridge模式]   │               │                  │
       │    注入 system     │               │                  │
       │<───────────────────│               │                  │
       │                    │               │                  │
       │ Provider 返回文本   │               │                  │
       │                    │               │                  │
       │ 解析工具调用        │               │                  │
       │───────────────────>│ parseXml()   │                  │
       │                    │─────────────>│                  │
       │                    │<─────────────│                  │
       │<───────────────────│ ToolCalls    │                  │
       │                    │               │                  │
       │ 验证工具调用        │               │                  │
       │──────────────────────────────────────────────────────>│
       │                    │               │   validate()     │
       │<──────────────────────────────────────────────────────│
       │                    │               │ ValidationResult │
       │                    │               │                  │
       │ [校验失败时应用降级策略]            │                  │
       │───────────────────>│               │                  │
       │                    │               │                  │
```

---

## 7. API 接口详细说明

### 7.1 Chat Completions API

**端点**: `POST /chaynsapi/v1/chat/completions`

**请求示例 - 非流式**:
```bash
curl -X POST "http://localhost:5555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_API_KEY" \
  -d '{
    "model": "GPT-4o",
    "messages": [
      {"role": "system", "content": "You are a helpful assistant."},
      {"role": "user", "content": "Hello!"}
    ]
  }'
```

**请求示例 - 流式**:
```bash
curl -N -X POST "http://localhost:5555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_API_KEY" \
  -d '{
    "model": "GPT-4o",
    "stream": true,
    "messages": [
      {"role": "user", "content": "Write a short poem."}
    ]
  }'
```

**请求示例 - 带工具调用**:
```bash
curl -X POST "http://localhost:5555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "messages": [
      {"role": "user", "content": "What is the weather in Tokyo?"}
    ],
    "tools": [
      {
        "type": "function",
        "function": {
          "name": "get_weather",
          "description": "Get weather for a location",
          "parameters": {
            "type": "object",
            "properties": {
              "location": {"type": "string", "description": "City name"}
            },
            "required": ["location"]
          }
        }
      }
    ],
    "tool_choice": "auto"
  }'
```

**非流式响应格式**:
```json
{
  "id": "chatcmpl-xxx",
  "object": "chat.completion",
  "created": 1234567890,
  "model": "GPT-4o",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "Hello! How can I help you today?"
      },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 10,
    "completion_tokens": 8,
    "total_tokens": 18
  }
}
```

**流式响应格式 (SSE)**:
```
data: {"id":"chatcmpl-xxx","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant"}}]}

data: {"id":"chatcmpl-xxx","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"Hello"}}]}

data: {"id":"chatcmpl-xxx","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"!"}}]}

data: {"id":"chatcmpl-xxx","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}

data: [DONE]
```

### 7.2 Responses API

**端点**: `POST /chaynsapi/v1/responses`

**请求示例 - 简单输入**:
```bash
curl -X POST "http://localhost:5555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "input": "Tell me a joke."
  }'
```

**请求示例 - 带 instructions**:
```bash
curl -X POST "http://localhost:5555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "instructions": "You are a professional comedian.",
    "input": "Tell me a joke about programming."
  }'
```

**请求示例 - 续聊**:
```bash
curl -X POST "http://localhost:5555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "previous_response_id": "resp_abc123",
    "input": "Tell me another one."
  }'
```

**获取 Response**:
```bash
curl "http://localhost:5555/chaynsapi/v1/responses/resp_abc123"
```

**删除 Response**:
```bash
curl -X DELETE "http://localhost:5555/chaynsapi/v1/responses/resp_abc123"
```

### 7.3 Models API

**端点**: `GET /chaynsapi/v1/models`

```bash
curl "http://localhost:5555/chaynsapi/v1/models"
```

**响应格式**:
```json
{
  "object": "list",
  "data": [
    {
      "id": "GPT-4o",
      "object": "model",
      "created": 1234567890,
      "owned_by": "openai"
    },
    {
      "id": "Claude-3.5-Sonnet",
      "object": "model",
      "created": 1234567890,
      "owned_by": "anthropic"
    }
  ]
}
```

---

## 8. 错误响应

### 8.1 错误格式

```json
{
  "error": {
    "type": "bad_request",
    "code": "BadRequest",
    "message": "Missing required field: model",
    "detail": "The 'model' field is required in the request body"
  }
}
```

### 8.2 错误码对照表

| ErrorCode | HTTP Status | type 字符串 | 说明 |
|-----------|-------------|-------------|------|
| BadRequest | 400 | bad_request | 请求格式错误 |
| Unauthorized | 401 | unauthorized | 未授权 |
| Forbidden | 403 | forbidden | 禁止访问 |
| NotFound | 404 | not_found | 资源不存在 |
| Conflict | 409 | conflict | 并发冲突 |
| RateLimited | 429 | rate_limited | 限流 |
| Timeout | 504 | timeout | 请求超时 |
| ProviderError | 502 | provider_error | Provider 错误 |
| Internal | 500 | internal | 内部错误 |
| Cancelled | 499 | cancelled | 请求被取消 |

---

## 9. 会话追踪

### 9.1 Hash 模式（默认）

基于消息内容的 SHA256 哈希生成会话标识：

```cpp
// 计算 contextHash
std::string contextHash = sha256(systemPrompt + "||" + joinedMessages);
```

### 9.2 ZeroWidth 模式

使用零宽字符在助手消息中嵌入不可见的会话 ID：

```cpp
// 编码 sessionId 到零宽字符
std::string encoded = ZeroWidthEncoder::encode(sessionId);

// 嵌入到助手回复开头
std::string response = encoded + actualContent;

// 解码提取 sessionId
std::string extracted = ZeroWidthEncoder::decode(response);
```

---

## 10. 配置示例

### config.json

```json
{
  "listeners": [
    {
      "address": "0.0.0.0",
      "port": 5555
    }
  ],
  "app": {
    "threads_num": 4,
    "enable_gzip": true
  },
  "custom_config": {
    "session_tracking": {
      "mode": "hash"
    },
    "tool_call_validation": {
      "default_mode": "none"
    }
  }
}
```

---

## 11. 构建与运行

### 11.1 依赖

- C++17 或更高版本
- Drogon 框架
- JsonCpp
- OpenSSL
- PostgreSQL（可选）

### 11.2 构建步骤

```bash
cd aiapi/src
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 11.3 运行

```bash
cd aiapi/src/build
./aiapi
```

服务默认监听 `0.0.0.0:5555`
