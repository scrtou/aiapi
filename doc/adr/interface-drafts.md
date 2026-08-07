# RFC-001 派生文档 · 关键接口草案（interface-drafts）

> 本文承接 RFC-001 §4。**这些是草案，不是契约** —— 实现阶段可在不改 ADR 的前提下调整签名。
> 若某处调整会**违反** [`decisions/`](./decisions/) 中任一条 ADR，则必须先改 ADR、再改本文。

| 关联 | 链接 |
|---|---|
| 决策依据 | [`decisions/README.md`](./decisions/README.md) |
| 落地次序 | [`migration-plan.md`](./migration-plan.md) |

---

## 4. 关键接口草案（C++17）

### 4.1 拆解 APIinterface（解决 P3 / P4）

现状：单一接口 7 个方法，混合三类职责。拆为三个：

```cpp
// domain/ports/IChatProvider.h —— 只管生成，输入不可变
struct ProviderCapabilities {
    bool nativeToolCalls   = false;
    bool streaming         = true;
    bool serverSideHistory = false;   // chayns / retool 有上游线程
    int  maxContextTokens  = 0;
};

class IChatProvider {
public:
    virtual ~IChatProvider() = default;
    [[nodiscard]] virtual Result<Generation> generate(const ProviderRequest& req) = 0;
    [[nodiscard]] virtual ProviderCapabilities capabilities() const = 0;
};
```

```cpp
// domain/ports/IProviderLifecycle.h —— 生命周期与健康检查
class IProviderLifecycle {
public:
    virtual ~IProviderLifecycle() = default;
    virtual void initialize() = 0;
    [[nodiscard]] virtual Result<void> refreshTokens() = 0;
    [[nodiscard]] virtual Result<ModelCatalog> listModels() = 0;
};
```

```cpp
// domain/ports/IUpstreamThreadStore.h —— 上游会话映射
// 对应原 eraseChatinfoMap / transferThreadContext
class IUpstreamThreadStore {
public:
    virtual ~IUpstreamThreadStore() = default;
    virtual void unbind(std::string_view conversationId) = 0;
    virtual void rebind(std::string_view fromId, std::string_view toId) = 0;
};
```

**要点**：
- `generate` 接收 `const ProviderRequest&` 而非 `session_st&`；
  `ProviderRequest` 是为上游裁剪过的扁平 DTO，不含可变会话状态。
- `ProviderCapabilities` 用于替代当前散落各处的 `if (providerName == "xxx")` 硬判断。

---

### 4.2 ProviderBase 模板方法（解决 P6）

```cpp
// infrastructure/provider/ProviderBase.h
class ProviderBase : public IChatProvider {
public:
    ProviderBase(IAccountRepository& accounts, IMetricsSink& metrics, IClock& clock);

    // final：共性流程固定，子类不得覆盖
    [[nodiscard]] Result<Generation> generate(const ProviderRequest& req) final;

protected:
    // —— 差异点，子类必须实现 ——
    [[nodiscard]] virtual Result<HttpRequest>  buildRequest(const ProviderRequest&) = 0;
    [[nodiscard]] virtual Result<void>         applyAuth(HttpRequest&, const Account&) = 0;
    [[nodiscard]] virtual Result<ChunkOutcome> parseChunk(std::string_view raw) = 0;
    [[nodiscard]] virtual Error mapUpstreamError(int status, std::string_view body) = 0;

private:
    // —— 共性，且各自独立可单测 ——
    SseFramer            framer_;
    RetryPolicy          retry_;
    IAccountRepository&  accounts_;
    IMetricsSink&        metrics_;
    IClock&              clock_;
};
```

共性流程（基类内固定）：
账号选取 → 鉴权 → 发起请求 → 超时控制 → SSE 分帧 → 逐片解析
→ 错误映射 → 重试退避 → 指标上报 → 归一化返回。

> `parseChunk` 用 `std::string_view` 实现零拷贝解析——这是保留 C++17 的直接收益之一。

---

### 4.3 生成流水线（解决 P5）

将 `GenerationServiceEmitAndToolBridge.cpp` 的 2213 行拆为显式 stage：

```
NormalizeRequest → ResolveContinuity → ApplyHistoryBudget
→ EncodeToolDefinitions → CallProvider → DecodeToolCalls
→ ValidateToolCalls → NormalizeToolArgs → SanitizeOutput → Emit
```

```cpp
// application/pipeline/Stage.h
class Stage {
public:
    virtual ~Stage() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual Result<void> run(GenCtx& ctx) = 0;
};

class Pipeline {
public:
    template <typename S, typename... Args>
    Pipeline& emplace(Args&&... args) {          // 就地构造，避免调用方写 make_unique
        stages_.push_back(std::make_unique<S>(std::forward<Args>(args)...));
        return *this;
    }
    [[nodiscard]] Result<void> execute(GenCtx& ctx);   // 任一 stage 失败即短路
private:
    std::vector<std::unique_ptr<Stage>> stages_;
};
```

按能力动态裁剪：

```cpp
Pipeline p;
p.emplace<NormalizeRequest>()
 .emplace<ResolveContinuity>(continuityResolver)
 .emplace<ApplyHistoryBudget>(budget);

const auto caps = provider.capabilities();
if (!caps.nativeToolCalls) {           // 上游不支持原生工具调用才装桥接
    p.emplace<EncodeToolDefinitions>(encoder);
}
p.emplace<CallProvider>(provider);
if (!caps.nativeToolCalls) {
    p.emplace<DecodeToolCalls>(xmlCodec);
}
p.emplace<ValidateToolCalls>(validator)
 .emplace<NormalizeToolArgs>()
 .emplace<SanitizeOutput>()
 .emplace<Emit>(sink);
```

**收益**：每个 stage 可独立单测；日志与 tracing 统一在 `Pipeline::execute` 中植入。

---

### 4.4 组合根（解决 P2）

```cpp
// AppContext.h
struct AppContext {
    // —— infrastructure ——
    std::shared_ptr<IClock>                clock;
    std::shared_ptr<IAccountRepository>    accounts;
    std::shared_ptr<IChannelRegistry>      channels;
    std::shared_ptr<ISessionStore>         sessions;
    std::shared_ptr<IMetricsSink>          metrics;
    std::shared_ptr<IProviderFactory>      providers;

    // —— application ——
    std::shared_ptr<ChatCompletionUseCase> chat;
    std::shared_ptr<ResponsesUseCase>      responses;
    std::shared_ptr<AccountUseCase>        account;
    std::shared_ptr<ChannelUseCase>        channel;

    [[nodiscard]] static Result<AppContext> build(const Config& cfg);
};
```

Controller 通过构造函数持有 UseCase 引用，不再调用任何 `getInstance()`。

---

### 4.5 Result<T, Error>（C++17 实现）

用 `std::variant` 做判别式联合——**不需要**自建 optional/expected 垫片：

```cpp
// platform/Result.h
struct Error {
    enum class Code {
        Network, Auth, RateLimited, InvalidRequest,
        Timeout, ServiceUnavailable, Internal, Unknown
    };
    Code        code = Code::Unknown;
    std::string message;
    std::string upstreamCode;   // 上游原始错误码
    int         httpStatus = 0;
};

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T v)      : data_(std::move(v)) {}
    Result(Error e)  : data_(std::move(e)) {}

    [[nodiscard]] bool ok() const noexcept {
        return std::holds_alternative<T>(data_);
    }
    explicit operator bool() const noexcept { return ok(); }

    const T&     value() const& { return std::get<T>(data_); }
    T&&          value() &&     { return std::get<T>(std::move(data_)); }
    const Error& error() const  { return std::get<Error>(data_); }

    // 函数式组合，减少层层 if (!r.ok()) return r.error();
    template <typename F>
    auto andThen(F&& f) -> decltype(f(std::declval<const T&>())) {
        if (!ok()) return error();
        return f(value());
    }

private:
    std::variant<T, Error> data_;
};

// void 特化
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error e) : error_(std::move(e)) {}
    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    const Error& error() const { return *error_; }
private:
    std::optional<Error> error_;
};
```

**相比 C++14 方案的优势**（保留 C++17 的直接收益）：
- 真正的判别式联合，不同时占用 T 与 Error 的空间
- 不要求 `T` 可默认构造
- `[[nodiscard]]` 使忽略错误变为编译警告
- `andThen` 让 Pipeline 各 stage 的错误传播链条简洁

---

