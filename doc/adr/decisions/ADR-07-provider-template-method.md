# ADR-07 Provider 必须继承薄 ProviderBase，并使用可组合策略

> 文件名为历史兼容保留。v3.0 保留 ProviderBase，但撤销“基类统一完整 HTTP 模板”的做法。

| 项 | 值 |
|---|---|
| 状态 | 已接受，已实施（P6-W3/P8-W1） |
| 当前版本 | v3.2 |

## Provider 范围

- 保留：`chayns`、`retool`；
- 下线：`nexos`、不可触达的 `OpenAiProvider`；
- OpenAI 兼容的 `/v1/chat/completions`、`/v1/responses` 协议保留。“删除 OpenAiProvider”不等于删除公开协议。

## 决策

所有生产 Provider 必须继承 infrastructure 层的薄 `ProviderBase`。application 只依赖 `IChatProvider`，不得依赖 `ProviderBase`。测试 fake 可以直接实现 `IChatProvider`，以便独立测试 application。

先统一边界，不预设共同 IO 流程：

```cpp
struct ProviderCallContext {
    const CancellationToken& cancellation; // Provider 只能观察，不能取消 caller
    Deadline deadline;
    GenerationEventSink& sink;
};

class IChatProvider {
public:
    virtual ~IChatProvider() = default;
    virtual Result<ProviderResponse> generate(
        const ProviderRequest&, ProviderCallContext&) = 0;
    virtual ProviderCapabilities capabilities() const = 0;
};
```

`ProviderBase::generate()` 使用 NVI 模式并标记 `final`，只负责公共边界：

```cpp
class ProviderBase : public IChatProvider {
public:
    Result<ProviderResponse> generate(
        const ProviderRequest&, ProviderCallContext&) final;

protected:
    virtual Result<ProviderResponse> doGenerate(
        const ProviderRequest&, ProviderCallContext&) = 0;
    virtual std::string_view providerName() const noexcept = 0;
};
```

- 请求不可变，不传 `session_st&`；
- deadline/cancellation 到达每个阻塞边界；
- Provider 返回结构化结果/事件，不写 `session.response`；
- Provider 不访问项目单例；
- 生命周期、模型目录、上游线程映射使用独立 port。

## ProviderBase 的边界

ProviderBase 统一 cancellation/deadline 前置检查、infrastructure 异常到 `Error` 的转换、tracing/metrics、结果合法性和单次错误上报。它不保存请求级可变状态，也不能在构造/析构中调用虚函数。

生产工厂用编译期约束确保实现没有绕过基类：

```cpp
template<class T>
void registerProductionProvider(std::string name) {
    static_assert(std::is_base_of_v<ProviderBase, T>,
                  "production provider must derive from ProviderBase");
    // 注册 T 的工厂函数
}
```

chayns 使用 JSON 轮询，retool 有 workflow/agent 两种模式，不存在稳定一致的“构造请求 → SSE → parseChunk”流程。把完整 HTTP 流程放入模板会制造空钩子。

`HttpTimeoutPolicy`、`RetryPolicy`、`ProviderErrorMapper`、`AccountSelector`、`PollingPolicy` 和 metrics decorator 作为独立组合对象。ProviderBase 是公共边界骨架，不负责这些策略的具体协议流程。

## 实施进度

P6-W1 已提供 JSON-free 的 `ProviderRequest/Response/Capabilities/CallContext`、`IChatProvider`、
`ProviderBase::generate() final`、异常转换/失败一次上报以及 `makeProductionProvider<T>()` 的
`static_assert`。`ProviderCallContext` 当前只持有只读 token + absolute deadline；上面草案中的 sink
仍会和真实 event publication path 一起在 P7 接入，不能为凑接口提前建立空转发钩子。

P6-W2 已让 `chaynsapi` 直接继承 `ProviderBase`，并同时实现只读模型目录
`IProviderModelCatalog` 与上游线程所有权 `IProviderThreadContext`。composition root 用生产 factory
构造/`initialize()` 后只以 `registerChatProvider` 发布它；Chayns 不再包含 `Session.h`、不访问
`APIinterface/session_st/session.response` 或项目 singleton。它把 HTTP timeout 限制在 request 的剩余
deadline 内，并在账号单飞等待、重试和 polling 边界观察只读 cancellation token。

`SessionExecutionGate` 保有 `CancellationSource`；CancelPrevious 安装新 lease 时取消旧 source，旧 guard
之后完成也不能释放新 lease。GenerationService、session cleanup/transfer、model catalog 与 reaper 已分别
通过 chat/model/thread 窄能力接线。

P6-W3 已让 `retoolapi` 直接继承 `ProviderBase`，实现同一模型目录和 thread-context capability；workflow/
agent 均从 `doGenerate(ProviderRequest, ProviderCallContext)` 返回 `Result<ProviderResponse>`。workspace/
thread affinity 为 adapter 私有状态，调用方只能通过 string-only `routingHints` 提供显式 workspace selector。
所有 Retool HTTP timeout 由 context 剩余 deadline 剪裁，polling/sleep 在下一阻塞边界检查 cancellation。
composition root 用 `makeProductionProvider<retoolapi>()`、`initialize()` 与 `registerChatProvider()` 接线；
`APIinterface`、`findProvider()`、`registerProvider()` 及 legacy registry lane 已删除。

## 迁移顺序

1. [完成] 为活跃 Chayns 建立离线 transport/continuation/Pro/timeout/cancellation 契约测试；
2. [完成] 让 Chayns 直接满足瘦 port，并删除其 session 副作用出口；
3. [完成] 切换 application、model catalog、thread cleanup/reaper 与 transport Error 接线；
4. [完成] 迁移 Retool 并删除旧 registry lane；
5. 比较两家真实重复代码，再抽公共策略；
6. [完成] 删除旧 `APIinterface`、ProviderResult compatibility 与 legacy registry lane。

不再迁移准备删除的 OpenAiProvider/nexos 作为样板。

## 验收

- 两家通过同一 port contract suite；
- Provider 中 `session.response` 和项目单例访问为 0；
- 所有同步 HTTP 都受 deadline 限制；
- 取消与断连传播到轮询/重试边界；
- 新增生产 Provider 必须继承 ProviderBase，只需实现 `doGenerate()` 和能力声明；测试 fake 只需实现端口，不以行数作为架构指标。
