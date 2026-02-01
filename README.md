# aiapi

基于 Drogon 框架的 AI API 网关服务，提供 OpenAI 兼容的 Chat Completions 和 Responses API 接口。

## 功能特性

- ✅ OpenAI Chat Completions API 兼容
- ✅ OpenAI Responses API 兼容
- ✅ 流式与非流式输出支持
- ✅ 多 Provider 支持（可扩展）
- ✅ 工具调用（Tool Calls）支持
- ✅ 工具调用桥接（Text Bridge）- 支持不原生支持工具调用的模型
- ✅ 工具调用验证（ToolCallValidator）- 支持 None/Relaxed/Strict 三种校验模式
- ✅ 会话追踪（Hash / ZeroWidth 两种模式）
- ✅ 并发门控（SessionExecutionGate）
- ✅ 输出清洗（ClientOutputSanitizer）
- ✅ 统一错误模型（Errors）

## 架构概览

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

## 项目结构

```
aiapi/src/
├── main.cc                     # 程序入口
├── config.json                 # Drogon 配置
│
├── controllers/                # HTTP 控制器
│   ├── AiApi.h / AiApi.cc     # 主路由控制器
│   └── sinks/                  # 输出 Sink 实现
│       ├── ChatJsonSink.cpp    # Chat 非流式输出
│       ├── ChatSseSink.cpp     # Chat 流式 SSE
│       └── ResponsesSseSink.cpp
│
├── sessionManager/             # 核心业务逻辑
│   ├── GenerationRequest.h     # 统一请求结构
│   ├── GenerationEvent.h       # 统一事件模型
│   ├── IResponseSink.h         # 输出通道接口
│   ├── RequestAdapters.h/cpp   # 请求适配器
│   ├── GenerationService.h/cpp # 生成编排服务
│   ├── Session.h/cpp           # 会话管理
│   ├── SessionExecutionGate.h/cpp # 并发门控
│   ├── ToolCallBridge.h/cpp    # 工具调用桥接
│   ├── XmlTagToolCallCodec.h/cpp # XML 编解码
│   ├── ToolCallValidator.h/cpp # 工具调用验证
│   ├── ClientOutputSanitizer.h/cpp # 输出清洗
│   └── Errors.h                # 统一错误模型
│
├── apipoint/                   # Provider 抽象
│   ├── APIinterface.h          # Provider 接口
│   ├── ProviderResult.h        # 结果结构
│   └── chaynsapi/              # chayns Provider 实现
│
├── apiManager/                 # Provider 管理
│   ├── ApiFactory.h/cpp        # Provider 工厂
│   └── ApiManager.h/cpp        # Provider 路由
│
├── accountManager/             # 账号管理
├── channelManager/             # 渠道管理
├── dbManager/                  # 数据库管理
│
└── tools/                      # 工具类
    └── ZeroWidthEncoder.h/cpp  # 零宽字符编码
```

## 核心模块说明

### GenerationEvent（统一事件模型）

所有生成过程产生的事件都通过 `GenerationEvent` 统一表示：

| 事件类型 | 说明 | 数据 |
|----------|------|------|
| `Started` | 生成开始 | responseId, model |
| `OutputTextDelta` | 文本增量（流式） | delta, index, outputItemIndex(可选) |
| `OutputTextDone` | 文本完成 | text, index |
| `ToolCallDone` | 工具调用完成 | id, name, arguments, index |
| `Usage` | Token 使用量 | inputTokens, outputTokens |
| `Completed` | 生成完成 | finishReason, usage(可选) |
| `Error` | 错误 | code, message, detail |

### IResponseSink（输出通道接口）

将 `GenerationEvent` 转换为具体协议格式：

```cpp
class IResponseSink {
    virtual void onEvent(const generation::GenerationEvent& event) = 0;
    virtual void onClose() = 0;
    virtual std::string getSinkType() const = 0;
};
```

内置实现：
- `ChatJsonSink` - Chat Completions 非流式 JSON
- `ChatSseSink` - Chat Completions 流式 SSE  
- `ResponsesSseSink` - Responses API 流式 SSE
- `NullSink` - 丢弃输出（测试用）
- `CollectorSink` - 收集事件（测试用）

### GenerationService（生成编排服务）

核心编排服务，负责整个生成流程：

```cpp
class GenerationService {
public:
    // 主入口：带并发门控的生成
    void runGuarded(
        const GenerationRequest& request,
        std::shared_ptr<IResponseSink> sink,
        session::ConcurrencyPolicy policy
    );
    
private:
    session_st materializeSession(const GenerationRequest& request);
    std::string computeExecutionKey(const session_st& session);
    ProviderResult executeProvider(session_st& session, const GenerationRequest& request);
    void emitResultEvents(const ProviderResult& result, std::shared_ptr<IResponseSink> sink);
    std::string sanitizeOutput(const std::string& output, const GenerationRequest& request);
    void processOutputWithBridge(const std::string& output, ...);
    std::vector<generation::ToolCallDone> parseXmlToolCalls(const std::string& text);
    generation::ToolCallDone generateForcedToolCall(const Json::Value& tools);
    void normalizeToolCallArguments(generation::ToolCallDone& call, const Json::Value& toolDef);
    bool selfHealReadFile(generation::ToolCallDone& call);
    void applyStrictClientRules(generation::ToolCallDone& call, const Json::Value& clientInfo);
};
```

### SessionExecutionGate（并发门控）

防止同一会话并发执行：

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

class SessionExecutionGate {
public:
    static SessionExecutionGate& getInstance();
    GateResult tryAcquire(const std::string& sessionKey, ConcurrencyPolicy policy, CancellationTokenPtr& outToken);
    void release(const std::string& sessionKey);
    bool isExecuting(const std::string& sessionKey) const;
    void cleanup(size_t maxIdleSlots = 1000);
};

// RAII 风格的执行守卫
class ExecutionGuard {
public:
    ExecutionGuard(const std::string& sessionKey, ConcurrencyPolicy policy = ConcurrencyPolicy::RejectConcurrent);
    ~ExecutionGuard();  // 自动释放
    bool isAcquired() const;
    GateResult getResult() const;
    CancellationTokenPtr getToken() const;
    bool isCancelled() const;
};

} // namespace session
```

### ToolCallBridge（工具调用桥接）

为不原生支持工具调用的模型提供桥接：

- **Native 模式**：直接透传原生 tool calls
- **TextBridge 模式**：
  - 请求侧：将工具定义转换为 XML 注入 system prompt
  - 响应侧：解析模型输出中的 XML 工具调用

### ToolCallValidator（工具调用验证）

验证模型生成的工具调用是否符合工具定义，支持三种校验模式：

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
    
    static ValidationResult success();
    static ValidationResult failure(const std::string& msg);
};

class ToolCallValidator {
public:
    // 构造时传入工具定义和客户端类型
    explicit ToolCallValidator(const Json::Value& toolDefs, const std::string& clientType = "");
    
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

// 辅助函数
bool isStrictToolClient(const std::string& clientType);
ValidationMode getRecommendedValidationMode(const std::string& clientType);

} // namespace toolcall
```

### ClientOutputSanitizer（输出清洗）

修正模型常见的输出错误：

- 修正标签拼写错误
- 去除非法控制字符
- 根据客户端类型应用不同的清洗规则

### Session（会话追踪）

支持两种追踪模式：

| 模式 | 实现 | 说明 |
|------|------|------|
| Hash | 消息内容 SHA256 | 默认模式，简单可靠 |
| ZeroWidth | 零宽字符嵌入 | 对用户不可见的 sessionId |

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

### Errors（统一错误模型）

统一的错误类型与 HTTP 状态码映射：

```cpp
namespace error {

enum class ErrorCode {
    None, BadRequest, Unauthorized, Forbidden, NotFound,
    Conflict, RateLimited, Timeout, ProviderError, Internal, Cancelled
};

struct AppError {
    ErrorCode code;
    std::string message;
    std::string detail;
    std::string providerCode;
    
    bool hasError() const;
    int httpStatus() const;
    std::string type() const;
    
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

## API 接口

### Chat Completions API

```bash
# 非流式
curl -X POST "http://localhost:5555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "messages": [{"role": "user", "content": "Hello"}]
  }'

# 流式
curl -N -X POST "http://localhost:5555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "stream": true,
    "messages": [{"role": "user", "content": "Hello"}]
  }'
```

### Responses API

```bash
# 创建 Response
curl -X POST "http://localhost:5555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "input": "Hello"
  }'

# 获取 Response
curl "http://localhost:5555/chaynsapi/v1/responses/{response_id}"

# 删除 Response  
curl -X DELETE "http://localhost:5555/chaynsapi/v1/responses/{response_id}"
```

### Models API

```bash
curl "http://localhost:5555/chaynsapi/v1/models"
```

## 构建与运行

### 依赖

- C++17 或更高版本
- Drogon 框架
- JsonCpp
- OpenSSL
- PostgreSQL（可选，用于持久化）

### 构建

```bash
cd aiapi/src
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 运行

```bash
cd aiapi/src/build
./aiapi
```

服务默认监听 `0.0.0.0:5555`

## 配置

配置文件位于 `aiapi/src/config.json`：

```json
{
  "listeners": [
    { "address": "0.0.0.0", "port": 5555 }
  ],
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

## 详细文档

- [调用关系图与接口样例](doc/aiapi_callflow_and_api_examples.md) - 详细的模块拆解、时序图和 curl 示例

## 错误码

| 错误码 | HTTP Status | 说明 |
|--------|-------------|------|
| BadRequest | 400 | 请求格式错误 |
| Unauthorized | 401 | 未授权 |
| Forbidden | 403 | 禁止访问 |
| NotFound | 404 | 资源不存在 |
| Conflict | 409 | 并发冲突 |
| RateLimited | 429 | 限流 |
| Timeout | 504 | 超时 |
| ProviderError | 502 | Provider 错误 |
| Internal | 500 | 内部错误 |
| Cancelled | 499 | 请求被取消 |

## 开发路线

- [x] Chat Completions API 基础功能
- [x] Responses API 基础功能
- [x] 流式输出支持
- [x] 工具调用支持
- [x] 工具调用桥接
- [x] 工具调用验证（ToolCallValidator - 支持 None/Relaxed/Strict 模式）
- [x] 会话追踪（Hash/ZeroWidth）
- [x] 并发门控（SessionExecutionGate + CancellationToken）
- [x] 输出清洗（ClientOutputSanitizer）
- [x] 统一错误模型（Errors）
- [ ] 真正的流式 Provider 回调
- [ ] 更多 Provider 实现
- [ ] 完善单元测试

## License

MIT
