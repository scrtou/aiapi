# aiapi 调用关系图与接口样例

本文档详细描述 aiapi 的模块拆解、调用关系、时序图以及 curl 测试示例。

---

## 1. 模块拆解

### 1.1 HTTP 层

| 模块 | 文件 | 职责 |
|------|------|------|
| AiApi Controller | `controllers/AiApi.h/cc` | HTTP 路由分发（22 个端点），请求预处理，响应构建 |
| ChatJsonSink | `controllers/sinks/ChatJsonSink.cpp` | Chat Completions 非流式 JSON 输出 |
| ChatSseSink | `controllers/sinks/ChatSseSink.cpp` | Chat Completions 流式 SSE 输出 |
| ResponsesJsonSink | `controllers/sinks/ResponsesJsonSink.cpp` | Responses API 非流式 JSON 输出 |
| ResponsesSseSink | `controllers/sinks/ResponsesSseSink.cpp` | Responses API 流式 SSE 输出 |

### 1.2 适配层

| 模块 | 文件 | 职责 |
|------|------|------|
| RequestAdapters | `sessionManager/RequestAdapters.h/cpp` | HTTP Request → GenerationRequest 转换（Chat/Responses 两种协议） |

### 1.3 生成编排层

| 模块 | 文件 | 职责 |
|------|------|------|
| GenerationService | `sessionManager/GenerationService.h/cpp` | 核心编排服务：门控→物化→Provider调用→结果处理→事件发送 |
| GenerationRequest | `sessionManager/GenerationRequest.h` | 统一请求结构（协议无关） |
| GenerationEvent | `sessionManager/GenerationEvent.h` | 统一事件模型（Started/TextDelta/TextDone/ToolCallDone/Usage/Completed/Error） |
| IResponseSink | `sessionManager/IResponseSink.h` | 输出通道接口 + CollectorSink/NullSink 内置实现 |
| Session | `sessionManager/Session.h/cpp` | 会话管理：创建/续接/转移/ZeroWidth嵌入/Hash追踪 |
| ContinuityResolver | `sessionManager/ContinuityResolver.h` | 会话连续性决策器：决定 sessionId 来源和追踪模式 |
| ResponseIndex | `sessionManager/ResponseIndex.h` | 响应存储索引（Responses API GET/DELETE 支持） |
| SessionExecutionGate | `sessionManager/SessionExecutionGate.h` | 并发门控：基于 sessionId 的分布式锁 + CancellationToken |
| ToolCallBridge | `sessionManager/ToolCallBridge.h/cpp` | 工具调用桥接：Native / TextBridge 两种模式 |
| XmlTagToolCallCodec | `sessionManager/XmlTagToolCallCodec.h/cpp` | XML 格式工具调用编解码（支持 Sentinel 机制） |
| ToolCallValidator | `sessionManager/ToolCallValidator.h/cpp` | 工具调用 Schema 校验 + 降级策略 |
| ClientOutputSanitizer | `sessionManager/ClientOutputSanitizer.h/cpp` | 输出清洗：修复标签/去除控制字符/客户端适配 |
| Errors | `sessionManager/Errors.h` | 统一错误模型：10 种错误码 + HTTP 状态码映射 |

### 1.4 Provider 层

| 模块 | 文件 | 职责 |
|------|------|------|
| APIinterface | `apipoint/APIinterface.h` | Provider 抽象接口（generate / getModels / afterResponseProcess） |
| ProviderResult | `apipoint/ProviderResult.h` | Provider 结果结构（text + statusCode + error） |
| ApiManager | `apiManager/ApiManager.h/cpp` | Provider 路由选择（单例） |
| ApiFactory | `apiManager/ApiFactory.h/cpp` | Provider 工厂（根据名称创建实例） |
| chaynsapi | `apipoint/chaynsapi/chaynsapi.h/cpp` | chayns Provider 具体实现 |

### 1.5 管理层

| 模块 | 文件 | 职责 |
|------|------|------|
| AccountManager | `accountManager/accountManager.h/cpp` | 账号池管理：CRUD/Token刷新/类型检测/自动注册/上游删除 |
| ChannelManager | `channelManager/channelManager.h/cpp` | 渠道管理：CRUD/状态控制/并发限制 |
| AccountDbManager | `dbManager/account/accountDbManager.h/cpp` | 账号数据库持久化 |
| ChannelDbManager | `dbManager/channel/channelDbManager.h/cpp` | 渠道数据库持久化 |
| ErrorStatsDbManager | `dbManager/metrics/ErrorStatsDbManager.h/cpp` | 错误统计数据库查询 |
| StatusDbManager | `dbManager/metrics/StatusDbManager.h/cpp` | 服务状态数据库查询 |
| ErrorStatsService | `metrics/ErrorStatsService.h` | 错误记录服务（recordError/recordWarn/recordRequestCompleted） |
| ZeroWidthEncoder | `tools/ZeroWidthEncoder.h/cpp` | 零宽字符编码/解码工具 |

---

## 2. 核心数据结构

### 2.1 GenerationRequest（统一请求结构）

```cpp
struct Message {
    MessageRole role;          // User, Assistant, System, Tool
    std::string content;
    std::string getTextContent() const;
};

enum class MessageRole { User, Assistant, System, Tool };

struct ImageInfo {
    std::string url;           // 图片 URL 或 base64 data URL
    std::string mimeType;      // "image/png", "image/jpeg" 等
    std::string base64;        // base64 编码数据
};

struct GenerationRequest {
    // 核心字段
    std::string model;                      // 模型名称
    std::string provider;                   // Provider 名称（默认 "chaynsapi"）
    std::string systemPrompt;               // 系统提示词
    std::string currentInput;               // 当前用户输入
    std::vector<Message> messages;          // 历史消息上下文
    std::vector<ImageInfo> images;          // 图片列表
    
    // 生成参数
    bool stream = false;                    // 是否流式输出
    std::optional<int> maxTokens;           // 最大 token 数
    std::optional<double> temperature;      // 温度参数
    
    // 工具调用
    Json::Value tools;                      // 工具定义（OpenAI function calling schema）
    std::string toolChoice;                 // 工具选择策略（auto/required/none/{...}）
    
    // 客户端信息
    Json::Value clientInfo;                 // 客户端元数据（client_type 等）
    std::string extractedSessionId;         // 从零宽字符提取的 session ID
    
    // Responses API
    std::optional<std::string> previousResponseId;  // 前一个响应 ID（续聊）
    
    // 协议判断
    bool isResponseApi() const;             // 是否为 Responses API 请求
};
```

### 2.2 GenerationEvent（统一事件模型）

```cpp
namespace generation {

// 生成开始事件
struct Started {
    std::string responseId;      // 响应 ID（Responses: resp_xxx; Chat: sessionId）
    std::string model;           // 使用的模型
};

// 文本增量事件（流式）
struct OutputTextDelta {
    std::string delta;           // 增量文本
    int index;                   // 输出项索引
    std::optional<int> outputItemIndex;
};

// 文本完成事件
struct OutputTextDone {
    std::string text;            // 完整文本
    int index;                   // 输出项索引
};

// 工具调用完成事件
struct ToolCallDone {
    std::string id;              // 调用 ID（如 call_xxx）
    std::string name;            // 工具名称（如 read_file）
    std::string arguments;       // 参数 JSON 字符串
    int index;                   // 工具调用索引
};

// Token 使用量事件
struct Usage {
    int inputTokens;
    int outputTokens;
};

// 生成完成事件
struct Completed {
    std::string finishReason;    // "stop" | "tool_calls" | "length"
    std::optional<Usage> usage;
};

// 错误事件
struct Error {
    ErrorCode code;              // 错误码枚举
    std::string message;
    std::string detail;
};

// 事件联合类型
using GenerationEvent = std::variant<
    Started, OutputTextDelta, OutputTextDone,
    ToolCallDone, Usage, Completed, Error
>;

} // namespace generation
```

### 2.3 session_st（会话结构）

```cpp
enum class ApiType { ChatCompletions, Responses };

struct session_st {
    // 会话标识
    std::string curConversationId;     // 当前会话 ID
    std::string nextSessionId;         // 预生成的下一轮 session ID
    bool is_continuation = false;      // 是否为续接会话
    
    // 请求内容
    std::string selectmodel;           // 选中的模型
    std::string selectapi;             // 选中的 Provider
    std::string systemprompt;          // 系统提示词
    std::string requestmessage;        // 请求消息（可能被 tool bridge 修改）
    std::string requestmessage_raw;    // 原始请求消息（tool bridge 注入前）
    std::vector<ImageInfo> requestImages;  // 请求图片
    
    // 工具调用
    Json::Value tools;                 // 工具定义（bridge 注入后可能被清除）
    Json::Value tools_raw;             // 原始工具定义（始终保留）
    std::string toolChoice;            // 工具选择策略
    std::string tool_bridge_trigger;   // Bridge 随机触发标记
    
    // 响应内容
    Json::Value responsemessage;       // Provider 返回的响应
    
    // 客户端信息
    Json::Value client_info;           // 客户端元数据
    
    // 协议与会话
    ApiType apiType;                   // API 类型
    std::string response_id;           // Responses API 的 responseId
    std::string lastResponseId;        // 上一个 responseId
    bool has_previous_response_id;     // 是否有 previous_response_id
    std::string request_id;            // 请求 ID（用于日志追踪）
    
    // 会话上下文
    Json::Value message_context;       // 历史消息上下文
    time_t last_active_time;           // 最后活动时间
    time_t created_time;               // 创建时间
    
    bool isResponseApi() const;        // 判断 API 类型
    void addMessageToContext(const Json::Value& msg);  // 添加消息到上下文
};
```

### 2.4 IResponseSink（输出通道接口）

```cpp
class IResponseSink {
public:
    virtual ~IResponseSink() = default;
    virtual void onEvent(const generation::GenerationEvent& event) = 0;
    virtual void onClose() = 0;
    virtual std::string getSinkType() const = 0;
};

// 内置实现
class CollectorSink : public IResponseSink {
    // 收集所有事件，供后续复放
    const std::vector<generation::GenerationEvent>& getEvents() const;
    bool hasError() const;
    std::optional<generation::Error> getError() const;
};

class NullSink : public IResponseSink {
    // 丢弃所有事件（测试用）
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
    
    bool hasError() const;
    int httpStatus() const;
    std::string type() const;
    
    // 工厂方法
    static AppError badRequest(...);
    static AppError unauthorized(...);
    static AppError forbidden(...);
    static AppError notFound(...);
    static AppError conflict(...);
    static AppError rateLimited(...);
    static AppError timeout(...);
    static AppError providerError(...);
    static AppError internal(...);
    static AppError cancelled(...);
};

} // namespace error
```

### 2.6 ProviderResult（Provider 结果结构）

```cpp
namespace provider {

struct ProviderResult {
    std::string text;            // 生成的文本
    int statusCode = 200;        // HTTP 状态码
    AppError error;              // 错误信息
    
    bool isSuccess() const { return statusCode >= 200 && statusCode < 300 && !error.hasError(); }
};

} // namespace provider
```

---

## 3. 工具调用系统

### 3.1 Tool Bridge（工具调用桥接）

#### 请求侧转换 (`transformRequestForToolBridge`)

当通道不支持原生 tool calls 时，将工具定义注入到用户消息中：

```
原始 requestmessage + "\n\n回答时必须满足..." + <tool_instructions>
  ├─ Context: Software engineering collaboration
  ├─ Task/Goal 说明（严格客户端 vs 普通客户端）
  ├─ 触发标记示例: <Function_Ab1c_Start/>
  ├─ XML 格式模板: <function_calls>/<function_call>/<tool>/<args_json>
  ├─ API Definitions（compact/full 模式）
  └─ </tool_instructions>
```

工具定义编码支持两种模式：

**compact 模式**（默认）：
```
Tool: read_file
Args:
  files : array [required]

Tool: edit_file
Args:
  file_path : string [required]
  old_string : string [required]
  new_string : string [required]
  expected_replacements : number [required]
```

**full 模式**：
```
Tool: read_file
Args:
  files : [{path}] [required]

Tool: edit_file
Args:
  file_path : string [required]
  old_string : string [required]
  new_string : string [required]
  expected_replacements : number [required]
```

#### 响应侧解析

```
上游返回文本
  │
  ├─ extractXmlInputForToolCalls()
  │   ├─ 优先查找 session.tool_bridge_trigger (随机标记)
  │   └─ 兜底查找 <function_calls> 标签
  │
  ├─ normalizeBridgeXml()
  │   ├─ 规范化换行符 (\r\n → \n)
  │   ├─ 替换 NBSP (U+00A0) → 空格
  │   └─ 替换全角空格 (U+3000) → 空格
  │
  └─ parseXmlToolCalls()
      ├─ XmlTagToolCallCodec.setSentinel(trigger)
      ├─ bridge.transformResponseChunk(xmlInput, events)
      ├─ bridge.flushResponse(events)
      └─ 处理事件: Text → textContent, ToolCallEnd → ToolCallDone[]
```

### 3.2 ToolCallValidator（工具调用验证）

```cpp
namespace toolcall {

enum class ValidationMode {
    None,      // 不校验 - 信任 AI 输出
    Relaxed,   // 宽松校验 - 关键字段非空（path, content 等）
    Strict     // 严格校验 - 完整 schema（required + 类型检查）
};

struct ValidationResult {
    bool valid = false;
    std::string errorMessage;
};

class ToolCallValidator {
public:
    explicit ToolCallValidator(const Json::Value& toolDefs, const std::string& clientType = "");
    
    ValidationResult validate(const generation::ToolCallDone& toolCall, ValidationMode mode) const;
    size_t filterInvalidToolCalls(
        std::vector<generation::ToolCallDone>& toolCalls,
        std::string& discardedText,
        ValidationMode mode
    ) const;
    
    bool hasToolDefinition(const std::string& toolName) const;
    const std::unordered_set<std::string>& getValidToolNames() const;
};

// 校验模式自动选择
ValidationMode getRecommendedValidationMode(const std::string& clientType);
// - RooCode/Kilo-Code → Relaxed
// - 其他 → None

// 降级策略
enum class FallbackStrategy { DiscardOnly, WrapAttemptCompletion, GenerateReadFile };
FallbackStrategy applyValidationFallback(
    const std::string& clientType,
    std::vector<generation::ToolCallDone>& toolCalls,
    std::string& textContent,
    const std::string& discardedText
);
// - 非严格客户端 → DiscardOnly（仅丢弃）
// - 严格客户端 → WrapAttemptCompletion（包装为 attempt_completion）

} // namespace toolcall
```

### 3.3 参数形状规范化 (`normalizeToolCallArguments`)

处理上游模型输出的常见参数格式问题：

| 问题 | 输入 | 输出 |
|------|------|------|
| 数组元素类型错误 | `files: ["src/main.cpp"]` | `files: [{path: "src/main.cpp"}]` |
| 参数别名 | `paths: ["src/main.cpp"]` | `files: [{path: "src/main.cpp"}]` |
| 缺失 mode 字段 | `follow_up: [{text: "Yes"}]` | `follow_up: [{text: "Yes", mode: ""}]` |
| 非法 mode 值 | `mode: "single"` | `mode: ""` |
| 对象键别名 | `{file: "main.cpp"}` | `{path: "main.cpp"}` |

### 3.4 强制工具调用 (`generateForcedToolCall`)

当 `tool_choice=required` 但上游未返回工具调用时的兜底机制：

1. 解析 toolChoice（支持字符串 `"required"` 和对象 `{"type":"function","function":{"name":"xxx"}}`）
2. 选择目标工具（指定名称 > 唯一工具 > 第一个工具）
3. 从用户输入自动提取参数（支持中英文地名提取等启发式规则）
4. 生成 `call_xxx` 格式的工具调用 ID

### 3.5 严格客户端规则 (`applyStrictClientRules`)

仅适用于 Kilo-Code / RooCode 客户端：

| 规则 | 条件 | 行为 |
|------|------|------|
| 规则 1 | 无工具调用 + 有文本 | 将文本包装为 `attempt_completion` 工具调用 |
| 规则 2 | 多个工具调用 | 只保留第一个 |

---

## 4. 并发门控系统

### 4.1 并发策略

```cpp
namespace session {

enum class ConcurrencyPolicy {
    RejectConcurrent,   // 拒绝并发请求（返回 409 Conflict）
    CancelPrevious      // 取消之前的请求，执行新请求
};

enum class GateResult {
    Acquired,           // 成功获取执行权
    Rejected,           // 被拒绝
    Cancelled           // 之前的请求被取消
};

} // namespace session
```

### 4.2 SessionExecutionGate（单例）

```cpp
class SessionExecutionGate {
public:
    static SessionExecutionGate& getInstance();
    GateResult tryAcquire(const std::string& sessionKey, ConcurrencyPolicy policy, CancellationTokenPtr& outToken);
    void release(const std::string& sessionKey);
    bool isExecuting(const std::string& sessionKey) const;
    void cleanup(size_t maxIdleSlots = 1000);
};
```

### 4.3 ExecutionGuard（RAII 守卫）

```cpp
class ExecutionGuard {
public:
    ExecutionGuard(const std::string& sessionKey, ConcurrencyPolicy policy);
    ~ExecutionGuard();               // 自动释放
    bool isAcquired() const;
    GateResult getResult() const;
    CancellationTokenPtr getToken() const;
    bool isCancelled() const;
};
```

### 4.4 CancellationToken

```cpp
class CancellationToken {
public:
    void cancel();                   // 请求取消
    bool isCancelled() const;        // 检查是否已取消
    void reset();                    // 重置
};
using CancellationTokenPtr = std::shared_ptr<CancellationToken>;
```

---

## 5. 会话连续性系统

### 5.1 ContinuityResolver

```cpp
enum class ContinuitySource {
    ZeroWidth,              // 从零宽字符提取的 sessionId
    PreviousResponseId,     // 从 previous_response_id 查找
    Hash,                   // 基于消息内容哈希
    New                     // 新建会话
};

enum class SessionTrackingMode { ZeroWidth, Hash };

struct ContinuityDecision {
    std::string sessionId;           // 决策的 sessionId
    ContinuitySource source;         // sessionId 来源
    SessionTrackingMode mode;        // 追踪模式
    std::string debug;               // 调试信息
};

class ContinuityResolver {
public:
    ContinuityDecision resolve(const GenerationRequest& req);
};
```

### 5.2 会话生命周期

```
请求到达
  │
  ├─ ContinuityResolver::resolve()
  │   ├─ 优先：extractedSessionId（从零宽字符提取）
  │   ├─ 次优：previousResponseId（Responses API 续聊）
  │   ├─ 兜底：Hash 模式或新建
  │   └─ 返回 ContinuityDecision
  │
  ├─ sessionManager.getOrCreateSession(sessionId, session)
  │   ├─ 存在 → 续接（is_continuation = true），恢复上下文
  │   └─ 不存在 → 新建（is_continuation = false）
  │
  ├─ [请求处理...]
  │
  ├─ prepareNextSessionId()    → 预生成下一轮 sessionId
  ├─ emitResultEvents()         → 嵌入 nextSessionId 到响应
  └─ coverSessionresponse()    → 会话转移（curConversationId → nextSessionId）
```

---

## 6. 错误统计系统

### 6.1 错误域

| 域 | 说明 | 典型事件类型 |
|------|------|----------|
| `SESSION_GATE` | 会话并发门控 | SESSIONGATE_REJECTED_CONFLICT, SESSIONGATE_CANCELLED |
| `UPSTREAM` | 上游 Provider | UPSTREAM_HTTP_ERROR |
| `TOOL_BRIDGE` | 工具桥接 | TOOLBRIDGE_XML_NOT_FOUND, TOOLBRIDGE_VALIDATION_FILTERED, TOOLBRIDGE_VALIDATION_FALLBACK_APPLIED, TOOLBRIDGE_STRICT_CLIENT_RULE_APPLIED, TOOLBRIDGE_FORCED_TOOLCALL_GENERATED, TOOLBRIDGE_TRANSFORM_INJECTED |
| `INTERNAL` | 内部异常 | INTERNAL_EXCEPTION, INTERNAL_UNKNOWN |

### 6.2 ErrorStatsService

```cpp
class ErrorStatsService {
public:
    static ErrorStatsService& getInstance();
    
    void recordError(Domain, type, message, requestId, provider, model, clientType, apiKind, stream, httpStatus, detail, rawSnippet, toolName);
    void recordWarn(...);
    void recordRequestCompleted(RequestCompletedData);
};
```

### 6.3 错误事件查询

```cpp
struct QueryParams {
    std::string from, to;                 // 时间范围
    std::string severity, domain, type;   // 过滤条件
    std::string provider, model;          // 按 provider/model 过滤
    std::string clientType;               // 按客户端类型过滤
};

struct ErrorEvent {
    int64_t id;
    std::string ts, severity, domain, type;
    std::string provider, model, clientType, apiKind;
    bool stream;
    int httpStatus;
    std::string requestId, message;
    std::string detailJson, rawSnippet;   // 仅详情查询返回
};
```

---

## 7. 调用关系图

### 7.1 整体架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                         HTTP 层                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    AiApi Controller                      │    │
│  │                                                          │    │
│  │  AI 核心:                                                │    │
│  │    POST /chaynsapi/v1/chat/completions                   │    │
│  │    POST /chaynsapi/v1/responses                          │    │
│  │    GET  /chaynsapi/v1/models                             │    │
│  │    GET  /chaynsapi/v1/responses/{id}                     │    │
│  │    DELETE /chaynsapi/v1/responses/{id}                   │    │
│  │                                                          │    │
│  │  账号管理:                                               │    │
│  │    POST /aichat/account/add|delete|update|refresh        │    │
│  │    POST /aichat/account/autoregister                     │    │
│  │    GET  /aichat/account/info|dbinfo                      │    │
│  │                                                          │    │
│  │  渠道管理:                                               │    │
│  │    POST /aichat/channel/add|delete|update|updatestatus   │    │
│  │    GET  /aichat/channel/info                             │    │
│  │                                                          │    │
│  │  监控:                                                   │    │
│  │    GET /aichat/metrics/requests/series                   │    │
│  │    GET /aichat/metrics/errors/series|events|events/{id}  │    │
│  │    GET /aichat/status/summary|channels|models            │    │
│  │    GET /aichat/logs/list|tail                            │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        适配层                                    │
│  ┌─────────────────┐     ┌──────────────────────────────────┐  │
│  │ RequestAdapters │ ──▶ │      GenerationRequest           │  │
│  │ (Chat/Responses)│     │ (协议无关的统一请求结构)           │  │
│  └─────────────────┘     └──────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      生成编排层                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │               GenerationService                          │    │
│  │  - runGuarded()          (新主入口)                       │    │
│  │  - materializeSession()  (请求 → 会话)                   │    │
│  │  - executeGuardedWithSession() (带门控的执行)             │    │
│  │  - executeProvider()     (调用上游)                       │    │
│  │  - emitResultEvents()    (结果处理 + 事件发送)            │    │
│  │  - transformRequestForToolBridge() (Bridge 注入)          │    │
│  │  - parseXmlToolCalls()   (XML 解析)                       │    │
│  │  - normalizeToolCallArguments() (参数规范化)               │    │
│  │  - applyStrictClientRules()   (严格客户端规则)             │    │
│  │  - generateForcedToolCall()   (强制工具调用兜底)           │    │
│  └─────────────────────────────────────────────────────────┘    │
│     │        │        │          │          │          │         │
│     ▼        ▼        ▼          ▼          ▼          ▼         │
│  ┌──────┐┌──────┐┌────────┐┌────────┐┌──────────┐┌──────────┐  │
│  │Tool  ││Conti-││Session ││ToolCall││Output    ││Response  │  │
│  │Call  ││nuity ││Execu-  ││Valida- ││Sanitizer ││Index     │  │
│  │Bridge││Resol-││tion    ││tor     ││          ││          │  │
│  │      ││ver   ││Gate    ││        ││          ││          │  │
│  └──────┘└──────┘└────────┘└────────┘└──────────┘└──────────┘  │
│     │                                                           │
│     ▼                                                           │
│  ┌──────────────────┐                                           │
│  │XmlTagToolCall    │                                           │
│  │Codec             │                                           │
│  └──────────────────┘                                           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Provider 层                                 │
│  ┌─────────────┐     ┌─────────────────────────────────────┐   │
│  │  ApiManager │ ──▶ │         APIinterface                │   │
│  │  (路由选择)  │     │  - generate(session) → ProviderResult│   │
│  │  ApiFactory  │     │  - getModels() → Json::Value        │   │
│  │  (工厂创建)  │     │  - afterResponseProcess(session)    │   │
│  └─────────────┘     └─────────────────────────────────────┘   │
│                            │                                     │
│                            └── chaynsapi (当前唯一实现)          │
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
│        ├── ChatJsonSink      (Chat 非流式 JSON)                  │
│        ├── ChatSseSink       (Chat 流式 SSE)                     │
│        ├── ResponsesJsonSink (Responses 非流式 JSON)              │
│        ├── ResponsesSseSink  (Responses 流式 SSE)                │
│        ├── CollectorSink     (事件收集，内部用)                   │
│        └── NullSink          (丢弃输出，测试用)                   │
└─────────────────────────────────────────────────────────────────┘
```

### 7.2 Chat Completions 请求处理时序图

```
Client              AiApi           RequestAdapters    GenerationService    Session         Provider
  │                   │                   │                   │                │               │
  │ POST /chat/...    │                   │                   │                │               │
  │──────────────────>│                   │                   │                │               │
  │                   │                   │                   │                │               │
  │                   │ JSON 解析 + 校验   │                   │                │               │
  │                   │                   │                   │                │               │
  │                   │ buildRequest()    │                   │                │               │
  │                   │──────────────────>│                   │                │               │
  │                   │<──────────────────│                   │                │               │
  │                   │ GenerationRequest │                   │                │               │
  │                   │                   │                   │                │               │
  │                   │ [非流式] ChatJsonSink / [流式] CollectorSink              │               │
  │                   │                   │                   │                │               │
  │                   │ runGuarded(req, sink, policy)         │                │               │
  │                   │──────────────────────────────────────>│                │               │
  │                   │                   │                   │                │               │
  │                   │                   │  materializeSession()              │               │
  │                   │                   │  (GenerationRequest → session_st)  │               │
  │                   │                   │                   │                │               │
  │                   │                   │  ContinuityResolver::resolve()     │               │
  │                   │                   │                   │                │               │
  │                   │                   │  getOrCreateSession()              │               │
  │                   │                   │                   │───────────────>│               │
  │                   │                   │                   │<───────────────│               │
  │                   │                   │                   │                │               │
  │                   │                   │  executeGuardedWithSession()       │               │
  │                   │                   │   │                │                │               │
  │                   │                   │   ├─ ExecutionGuard(RAII)          │               │
  │                   │                   │   │                │                │               │
  │                   │                   │   ├─ transformRequestForToolBridge()│               │
  │                   │                   │   │  (如果通道不支持原生 tool calls) │               │
  │                   │                   │   │                │                │               │
  │                   │                   │   ├─ sink.onEvent(Started)          │               │
  │                   │                   │   │                │                │               │
  │                   │                   │   ├─ executeProvider()              │               │
  │                   │                   │   │                │                │ generate()    │
  │                   │                   │   │                │                │──────────────>│
  │                   │                   │   │                │                │<──────────────│
  │                   │                   │   │                │                │ ProviderResult│
  │                   │                   │   │                │                │               │
  │                   │                   │   ├─ prepareNextSessionId()         │               │
  │                   │                   │   │                │                │               │
  │                   │                   │   ├─ emitResultEvents()             │               │
  │                   │                   │   │  ├─ sanitizeOutput()            │               │
  │                   │                   │   │  ├─ parseXmlToolCalls()         │               │
  │                   │                   │   │  ├─ normalizeToolCallArguments()│               │
  │                   │                   │   │  ├─ ToolCallValidator           │               │
  │                   │                   │   │  ├─ applyStrictClientRules()    │               │
  │                   │                   │   │  ├─ 零宽字符嵌入                 │               │
  │                   │                   │   │  └─ sink.onEvent(ToolCallDone/TextDone/Completed)
  │                   │                   │   │                │                │               │
  │                   │                   │   ├─ coverSessionresponse()         │               │
  │                   │                   │   │                │───────────────>│               │
  │                   │                   │   │                │<───────────────│               │
  │                   │                   │   │                │                │               │
  │                   │                   │   ├─ afterResponseProcess()         │               │
  │                   │                   │   │                │                │               │
  │                   │                   │   ├─ recordRequestCompletedStat()   │               │
  │                   │                   │   │                │                │               │
  │                   │                   │   └─ sink.onClose()                 │               │
  │                   │                   │                   │                │               │
  │                   │<──────────────────────────────────────│                │               │
  │                   │                   │                   │                │               │
  │                   │ [流式] 复放事件到 ChatSseSink          │                │               │
  │                   │ → StreamResponse                      │                │               │
  │                   │                   │                   │                │               │
  │    HTTP Response  │                   │                   │                │               │
  │<──────────────────│                   │                   │                │               │
```

### 7.3 emitResultEvents 内部处理流程

```
emitResultEvents(session, sink)
  │
  ├─ Step 0: 初始化
  │   • text = session.responsemessage["message"]
  │   • clientType = session.client_info["client_type"]
  │   • strictToolClient = (Kilo-Code || RooCode)
  │
  ├─ Step 1: sanitizeOutput(clientInfo, text)
  │   • 修复 XML/HTML 标签错误
  │   • 移除不可见控制字符
  │   • 客户端特定格式调整
  │
  ├─ Step 2: Tool Call 解析
  │   ├─ [supportsToolCalls || toolChoiceNone]
  │   │   └─ textContent = text（直接使用）
  │   └─ [Bridge 模式]
  │       ├─ extractXmlInputForToolCalls(session, text)
  │       │   ├─ 查找 session.tool_bridge_trigger
  │       │   └─ 兜底查找 <function_calls>
  │       ├─ normalizeBridgeXml(xmlInput)
  │       ├─ 确定 Sentinel（触发标记匹配）
  │       ├─ parseXmlToolCalls(xmlInput, textContent, toolCalls, sentinel)
  │       └─ generateForcedToolCall()（如果 tool_choice=required 且无结果）
  │
  ├─ Step 3: normalizeToolCallArguments(session, toolCalls)
  │   • files: ["path"] → [{path: "path"}]
  │   • paths → files 别名
  │   • follow_up mode 规范化
  │
  ├─ Step 4: ToolCallValidator 校验 + 过滤
  │   ├─ 选择校验模式: getRecommendedValidationMode(clientType)
  │   ├─ filterInvalidToolCalls(toolCalls, discardedText, mode)
  │   └─ [全部过滤] applyValidationFallback(clientType, toolCalls, textContent, discardedText)
  │
  ├─ Step 5: applyStrictClientRules()（仅 Roo/Kilo）
  │   ├─ 无工具调用 + 有文本 → attempt_completion
  │   └─ 多个工具调用 → 只保留第一个
  │
  ├─ Step 7: 零宽字符会话 ID 嵌入
  │   ├─ [claudecode + 有工具调用] → 单独发送零宽 ID 事件
  │   └─ [其他] → 嵌入 textContent 末尾
  │
  └─ Step 8: 发送事件
      ├─ for tc : toolCalls → sink.onEvent(ToolCallDone)
      ├─ if !textContent.empty() → sink.onEvent(OutputTextDone)
      └─ sink.onEvent(Completed{finishReason: toolCalls.empty() ? "stop" : "tool_calls"})
```

---

## 8. API 接口详细说明

### 8.1 Chat Completions API

**端点**: `POST /chaynsapi/v1/chat/completions`

**请求体**:
```json
{
  "model": "GPT-4o",                    // 必填：模型名称
  "messages": [                          // 必填：消息数组
    {"role": "system", "content": "..."},
    {"role": "user", "content": "..."},
    {"role": "assistant", "content": "..."},
    {"role": "user", "content": "..."}
  ],
  "stream": false,                       // 可选：流式输出（默认 false）
  "tools": [...],                        // 可选：工具定义
  "tool_choice": "auto"                  // 可选：工具选择策略
}
```

**curl 示例**:

```bash
# 非流式
curl -X POST "http://localhost:5555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "messages": [
      {"role": "system", "content": "You are a helpful assistant."},
      {"role": "user", "content": "Hello!"}
    ]
  }'

# 流式
curl -N -X POST "http://localhost:5555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "stream": true,
    "messages": [{"role": "user", "content": "Write a short poem."}]
  }'

# 带工具调用
curl -X POST "http://localhost:5555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "messages": [{"role": "user", "content": "What is the weather in Tokyo?"}],
    "tools": [{
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
    }],
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
  "choices": [{
    "index": 0,
    "message": {
      "role": "assistant",
      "content": "Hello! How can I help you today?"
    },
    "finish_reason": "stop"
  }],
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

### 8.2 Responses API

**端点**: `POST /chaynsapi/v1/responses`

**请求体**:
```json
{
  "model": "GPT-4o",                       // 必填：模型名称
  "input": "Tell me a joke.",               // 必填：用户输入
  "instructions": "You are a comedian.",    // 可选：系统指令
  "stream": false,                          // 可选：流式输出
  "previous_response_id": "resp_abc123",    // 可选：续聊
  "tools": [...],                           // 可选：工具定义
  "tool_choice": "auto"                     // 可选：工具选择策略
}
```

**curl 示例**:

```bash
# 创建 Response（简单输入）
curl -X POST "http://localhost:5555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{"model": "GPT-4o", "input": "Tell me a joke."}'

# 带 instructions
curl -X POST "http://localhost:5555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "instructions": "You are a professional comedian.",
    "input": "Tell me a joke about programming."
  }'

# 续聊（previous_response_id）
curl -X POST "http://localhost:5555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "previous_response_id": "resp_abc123",
    "input": "Tell me another one."
  }'

# 流式
curl -N -X POST "http://localhost:5555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{"model": "GPT-4o", "stream": true, "input": "Hello"}'

# 获取 Response
curl "http://localhost:5555/chaynsapi/v1/responses/resp_abc123"

# 删除 Response
curl -X DELETE "http://localhost:5555/chaynsapi/v1/responses/resp_abc123"
```

### 8.3 Models API

**端点**: `GET /chaynsapi/v1/models`

```bash
curl "http://localhost:5555/chaynsapi/v1/models"
```

### 8.4 账号管理 API

```bash
# 添加账号（支持单个对象或数组）
curl -X POST "http://localhost:5555/aichat/account/add" \
  -H "Content-Type: application/json" \
  -d '{
    "apiname": "chaynsapi",
    "username": "user@example.com",
    "password": "xxx",
    "accounttype": "free"
  }'

# 批量添加
curl -X POST "http://localhost:5555/aichat/account/add" \
  -H "Content-Type: application/json" \
  -d '[{"apiname":"chaynsapi","username":"u1@ex.com","password":"p1"},{"apiname":"chaynsapi","username":"u2@ex.com","password":"p2"}]'

# 删除账号
curl -X POST "http://localhost:5555/aichat/account/delete" \
  -H "Content-Type: application/json" \
  -d '{"apiname": "chaynsapi", "username": "user@example.com"}'

# 更新账号
curl -X POST "http://localhost:5555/aichat/account/update" \
  -H "Content-Type: application/json" \
  -d '{"apiname":"chaynsapi","username":"user@example.com","password":"new_pwd"}'

# 刷新所有账号状态（异步）
curl -X POST "http://localhost:5555/aichat/account/refresh"
# 响应: {"status":"started","message":"Account status refresh started in background"}

# 自动注册（异步，最多 20 个/次，间隔 5 秒）
curl -X POST "http://localhost:5555/aichat/account/autoregister" \
  -H "Content-Type: application/json" \
  -d '{"apiname": "chaynsapi", "count": 5}'
# 响应: {"status":"started","message":"Auto registration started in background"}

# 获取内存中的账号列表
curl "http://localhost:5555/aichat/account/info"

# 获取数据库中的账号列表
curl "http://localhost:5555/aichat/account/dbinfo"
```

**账号信息字段**:
```json
{
  "apiname": "chaynsapi",
  "username": "user@example.com",
  "password": "xxx",
  "authtoken": "eyJ...",
  "usecount": 42,
  "tokenstatus": true,
  "accountstatus": true,
  "usertobitid": 12345,
  "personid": "xxx-xxx",
  "createtime": "2026-01-15 10:30:00",
  "accounttype": "free"
}
```

### 8.5 渠道管理 API

```bash
# 添加渠道
curl -X POST "http://localhost:5555/aichat/channel/add" \
  -H "Content-Type: application/json" \
  -d '[{
    "channelname": "main",
    "channeltype": "chaynsapi",
    "channelurl": "https://api.example.com",
    "channelkey": "sk-xxx",
    "channelstatus": true,
    "maxconcurrent": 10,
    "timeout": 30,
    "priority": 0,
    "description": "主渠道",
    "accountcount": 5,
    "supports_tool_calls": false
  }]'

# 获取渠道列表
curl "http://localhost:5555/aichat/channel/info"

# 更新渠道
curl -X POST "http://localhost:5555/aichat/channel/update" \
  -H "Content-Type: application/json" \
  -d '{"id":1,"channelname":"main","channeltype":"chaynsapi","channelurl":"https://api.example.com","channelkey":"sk-xxx","channelstatus":true,"maxconcurrent":20,"timeout":60,"priority":0,"accountcount":10,"supports_tool_calls":true}'

# 更新渠道状态（启用/禁用）
curl -X POST "http://localhost:5555/aichat/channel/updatestatus" \
  -H "Content-Type: application/json" \
  -d '{"channelname": "main", "status": false}'

# 删除渠道
curl -X POST "http://localhost:5555/aichat/channel/delete" \
  -H "Content-Type: application/json" \
  -d '[{"id": 1}]'
```

**渠道信息字段**:
```json
{
  "id": 1,
  "channelname": "main",
  "channeltype": "chaynsapi",
  "channelurl": "https://api.example.com",
  "channelkey": "sk-xxx",
  "channelstatus": true,
  "maxconcurrent": 10,
  "timeout": 30,
  "priority": 0,
  "description": "主渠道",
  "createtime": "2026-01-15 10:30:00",
  "updatetime": "2026-02-07 09:00:00",
  "accountcount": 5,
  "supports_tool_calls": false
}
```

### 8.6 监控 API

```bash
# 服务状态概览
curl "http://localhost:5555/aichat/status/summary"
# 可选参数: ?from=2026-02-06+00:00:00&to=2026-02-07+00:00:00

# 渠道状态列表
curl "http://localhost:5555/aichat/status/channels"
# 可选参数: ?provider=chaynsapi

# 模型状态列表
curl "http://localhost:5555/aichat/status/models"
# 可选参数: ?provider=chaynsapi&model=GPT-4o

# 请求量时序统计（默认最近 24 小时）
curl "http://localhost:5555/aichat/metrics/requests/series"

# 错误量时序统计（支持多维过滤）
curl "http://localhost:5555/aichat/metrics/errors/series?severity=error&domain=UPSTREAM&provider=chaynsapi"

# 错误事件列表（分页）
curl "http://localhost:5555/aichat/metrics/errors/events?limit=50&offset=0"

# 错误事件详情（含 detail_json 和 raw_snippet）
curl "http://localhost:5555/aichat/metrics/errors/events/42"
```

**状态概览响应**:
```json
{
  "total_requests": 1234,
  "total_errors": 56,
  "error_rate": 4.54,
  "channel_count": 3,
  "model_count": 5,
  "healthy_channels": 2,
  "degraded_channels": 1,
  "down_channels": 0,
  "overall_status": "degraded",
  "buckets": [
    {"bucket_start": "2026-02-07 00:00:00", "request_count": 100, "error_count": 5, "error_rate": 5.0}
  ]
}
```

### 8.7 日志 API

```bash
# 获取日志文件列表
curl "http://localhost:5555/aichat/logs/list"
# 响应: [{"name":"aiapi.log","size":10485760,"modified":"2026-02-07 09:55:00"}]

# 读取日志尾部（默认 200 行）
curl "http://localhost:5555/aichat/logs/tail"

# 指定文件、行数、级别过滤、关键词过滤
curl "http://localhost:5555/aichat/logs/tail?file=aiapi.log&lines=500&level=ERROR&keyword=Provider"
```

**日志尾部响应**:
```json
{
  "file": "aiapi.log",
  "total_lines": 5000,
  "returned_lines": 200,
  "lines": ["[2026-02-07 09:55:16] [INFO] ...", "..."],
  "timestamp": "2026-02-07 09:56:00"
}
```

---

## 9. 错误响应

### 9.1 错误格式

```json
{
  "error": {
    "type": "bad_request",
    "code": "invalid_json",
    "message": "Invalid JSON in request body"
  }
}
```

### 9.2 错误码对照表

| ErrorCode | HTTP Status | type 字符串 | 说明 |
|-----------|-------------|-------------|------|
| BadRequest | 400 | bad_request | 请求格式错误 |
| Unauthorized | 401 | unauthorized | 未授权 |
| Forbidden | 403 | forbidden | 禁止访问 |
| NotFound | 404 | not_found | 资源不存在 |
| Conflict | 409 | conflict | 并发冲突（同一会话已有请求在执行） |
| RateLimited | 429 | rate_limited | 限流 |
| Timeout | 504 | timeout | 请求超时 |
| ProviderError | 502 | provider_error | Provider 错误 |
| Internal | 500 | internal | 内部错误 |
| Cancelled | 499 | cancelled | 请求被取消 |

---

## 10. 会话追踪

### 10.1 Hash 模式（默认）

基于消息内容的 SHA256 哈希生成会话标识：

```cpp
std::string contextHash = sha256(systemPrompt + "||" + joinedMessages);
```

### 10.2 ZeroWidth 模式

使用零宽字符在助手消息中嵌入不可见的会话 ID：

```cpp
// 编码 sessionId 到零宽字符
std::string encoded = ZeroWidthEncoder::encode(sessionId);

// 嵌入到助手回复中
std::string response = chatSession::embedSessionIdInText(content, sessionId);

// 从用户下次请求的消息中提取 sessionId
std::string extracted = ZeroWidthEncoder::decode(message);
```

### 10.3 两阶段 Session 转移

1. **prepareNextSessionId()**：在 emitResultEvents 之前预生成 nextSessionId
2. **嵌入 nextSessionId**：零宽字符嵌入到响应文本中
3. **commitSessionTransfer()**：在 coverSessionresponse 中执行实际转移

---

## 11. 配置示例

### config.json

```json
{
  "listeners": [
    { "address": "0.0.0.0", "port": 5555, "https": false }
  ],
  "db_clients": [
    {
      "name": "aichatpg",
      "rdbms": "postgresql",
      "host": "YOUR_DB_HOST",
      "port": 14539,
      "dbname": "aichat",
      "user": "YOUR_DB_USER",
      "passwd": "YOUR_DB_PASSWORD"
    }
  ],
  "app": {
    "number_of_threads": 4,
    "log": {
      "use_spdlog": true,
      "log_path": "logs",
      "logfile_base_name": "aiapi",
      "log_level": "DEBUG"
    },
    "cors": {
      "enabled": true,
      "allow_origins": ["*"],
      "allow_methods": ["GET", "POST", "PUT", "DELETE", "OPTIONS", "PATCH"]
    }
  },
  "plugins": [
    { "name": "drogon::plugin::PromExporter", "config": { "path": "/metrics" } },
    { "name": "drogon::plugin::AccessLogger", "config": { "use_spdlog": true } }
  ],
  "custom_config": {
    "session_tracking": {
      "mode": "zerowidth"
    },
    "tool_bridge": {
      "definition_mode": "compact",
      "include_descriptions": false,
      "max_description_chars": 160
    },
    "login_service_urls": [
      { "name": "chaynsapi", "url": "http://127.0.0.1:5557/aichat/chayns/login" }
    ],
    "regist_service_urls": [
      { "name": "chaynsapi", "url": "http://127.0.0.1:5557/aichat/chayns/autoregister" }
    ]
  }
}
```

### 关键配置项

| 配置路径 | 说明 | 可选值 |
|----------|------|--------|
| `custom_config.session_tracking.mode` | 会话追踪模式 | `hash` / `zerowidth` |
| `custom_config.tool_bridge.definition_mode` | 工具定义编码模式 | `compact`（默认） / `full` |
| `custom_config.tool_bridge.include_descriptions` | 是否包含工具/参数描述 | `true` / `false`（默认） |
| `custom_config.tool_bridge.max_description_chars` | 描述截断长度 | 0-2000（默认 160） |
| `custom_config.login_service_urls` | 登录服务地址 | URL 数组 |
| `custom_config.regist_service_urls` | 注册服务地址 | URL 数组 |

---

## 12. 构建与运行

### 12.1 依赖

- C++17 或更高版本
- Drogon 框架
- JsonCpp
- OpenSSL
- spdlog
- PostgreSQL

### 12.2 本地构建

```bash
cd aiapi/src
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 12.3 运行

```bash
cd aiapi/src/build
./aiapi
```

服务默认监听 `0.0.0.0:5555`

### 12.4 Docker 构建

```bash
# 方式一：环境变量注入配置
docker compose -f docker-compose.env.yml up --build

# 方式二：卷挂载配置文件
cp config.example.json config.json
# 编辑 config.json
docker compose -f docker-compose.volume.yml up --build
```

Docker 入口脚本支持：
- `CONFIG_JSON` → 直接覆盖 `/usr/aiapi/src/build/config.json`
- `CUSTOM_CONFIG` → 使用 `jq -s '.[0] * .[1]'` 合并到现有配置
