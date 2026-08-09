# RFC-001 关键接口草案

> 本文是实现草案，不是独立决策。与 ADR 冲突时以 `decisions/` 为准；修改决策必须先更新 ADR。

## 1. 纯领域请求

domain 类型不包含 `Json::Value`。HTTP JSON 和上游 JSON 在边缘 codec 中转换。

```cpp
struct ToolDefinition {
    std::string name;
    std::string description;
    Schema schema;                  // 项目自有强类型 schema/值树
};

struct ProviderRequest {
    std::string conversationId;
    std::string model;
    std::string systemPrompt;
    std::vector<Message> messages;
    std::vector<Image> images;
    std::vector<ToolDefinition> tools;
    ToolChoice toolChoice;
    bool parallelToolCalls = true;
};
```

若一次性引入通用 `Schema` 代价过高，可先使用边缘层拥有的序列化字符串作为过渡，但不得把 JsonCpp 类型放回 domain port。

## 2. Result

```cpp
struct OkTag {};
struct ErrTag {};

template<class T>
class [[nodiscard]] Result {
public:
    static Result success(T value) {
        return Result(OkTag{}, std::move(value));
    }
    static Result failure(Error error) {
        return Result(ErrTag{}, std::move(error));
    }

    bool ok() const noexcept;
    explicit operator bool() const noexcept { return ok(); }
    const T& value() const&;
    T&& value() &&;
    const Error& error() const&;

private:
    Result(OkTag, T value) : data_(std::move(value)) {}
    Result(ErrTag, Error error) : data_(std::move(error)) {}
    std::variant<T, Error> data_;
};

template<>
class [[nodiscard]] Result<void> {
public:
    static Result success();
    static Result failure(Error error);
    bool ok() const noexcept;
    const Error& error() const&;
private:
    std::optional<Error> error_;
};
```

实现需要覆盖 move-only 值、错误态访问、`T == Error`、void 和 nodiscard 编译门禁。

## 3. Deadline 与取消

```cpp
using Deadline = std::chrono::steady_clock::time_point;

class CancellationToken {
public:
    void cancel() noexcept;
    bool isCancelled() const noexcept;
};

struct ProviderCallContext {
    CancellationToken& cancellation;
    Deadline deadline;
    GenerationEventSink& sink;

    std::chrono::milliseconds remaining() const;
};
```

所有 timeout 都从绝对 deadline 计算，禁止每层重新开始一个完整相对超时。

## 4. Provider ports

```cpp
struct ProviderCapabilities {
    bool nativeToolCalls = false;
    bool upstreamHistory = false;
    bool supportsImages = false;
};

struct ProviderResponse {
    std::string text;
    std::vector<ToolCall> toolCalls;
    std::optional<Usage> usage;
    ProviderMetadata metadata;
};

class IChatProvider {
public:
    virtual ~IChatProvider() = default;
    virtual Result<ProviderResponse> generate(
        const ProviderRequest&, ProviderCallContext&) = 0;
    virtual ProviderCapabilities capabilities() const = 0;
};

class IProviderCatalog {
public:
    virtual ~IProviderCatalog() = default;
    virtual Result<std::vector<ModelInfo>> listModels(Deadline) = 0;
};

class IUpstreamConversationStore {
public:
    virtual ~IUpstreamConversationStore() = default;
    virtual Result<void> unbind(std::string_view id) = 0;
    virtual Result<void> rebind(std::string_view from, std::string_view to) = 0;
};

// 所有生产 Provider 必须继承这个 infrastructure 层的薄基类。
// application 只依赖 IChatProvider；测试 fake 可以直接实现 IChatProvider。
class ProviderBase : public IChatProvider {
public:
    Result<ProviderResponse> generate(
        const ProviderRequest& request,
        ProviderCallContext& context) final;

protected:
    // 只统一边界，不规定 HTTP/轮询/SSE 流程。
    virtual Result<ProviderResponse> doGenerate(
        const ProviderRequest&, ProviderCallContext&) = 0;
    virtual std::string_view providerName() const noexcept = 0;
};
```

`ProviderBase::generate()` 的公共实现负责 cancellation/deadline 前置检查、异常转换、tracing/metrics、结果合法性检查和单次错误上报。ProviderBase 不保存请求级可变成员，也不包含 `buildRequest → send → parseChunk` 固定模板；Retry、轮询、账号选择、错误映射和 HTTP timeout 由具体 Provider 组合。

聊天、模型目录、生命周期和上游会话映射分开，避免重建当前多职责 `APIinterface`。

## 5. GenerationEventSink

```cpp
class GenerationEventSink {
public:
    virtual ~GenerationEventSink() = default;
    // ShuttingDown/Disconnected 会触发同一请求的 CancellationToken。
    virtual Result<void> emit(const GenerationEvent&) = 0;
};
```

Sink 的 transport 实现在创建它的 Drogon loop 上发送；worker 只调用线程安全代理。

## 6. Application Pipeline

```cpp
struct GenerationContext {
    GenerationRequest request;
    ProviderResponse providerResponse;
    CancellationToken& cancellation;
    Deadline deadline;
};

class Stage {
public:
    virtual ~Stage() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual Result<void> run(GenerationContext&) = 0;
};
```

Pipeline 当前保持同步，由 worker 执行。先从现有函数中抽取纯规则；不要先创建十个只有转发逻辑的空 Stage。

## 7. 队列

```cpp
enum class EnqueueResult {
    Accepted,
    QueueFull,
    ShuttingDown,
    Stopped
};

class ITaskExecutor {
public:
    virtual ~ITaskExecutor() = default;
    virtual EnqueueResult enqueue(Task task) = 0;
    virtual void beginDraining() = 0;
    virtual bool waitUntil(Deadline deadline) = 0;
};
```

`waitUntil(false)` 后不能析构仍被 worker 使用的 executor；由 AppContext 进入 ADR-08 定义的进程级超时兜底。

## 8. AppContext

```cpp
class AppContext {
public:
    static Result<std::unique_ptr<AppContext>> build(const ProcessConfig&);
    Result<void> shutdown(Deadline deadline); // 幂等

    ChatCompletionUseCase& chat();
    ResponsesUseCase& responses();

private:
    // 声明顺序不承担停机语义；shutdown 显式编排。
    Infrastructure infrastructure_;
    Application application_;
};
```

默认唯一所有权；只有异步跨任务共享时才提升为 `shared_ptr`。
