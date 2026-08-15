# 04. 协议扩展机制

## 1. 协议模块

每一种外部协议是一个独立模块，而不是散落在核心代码中的条件分支。

```cpp
class IProtocolModule {
public:
    virtual ~IProtocolModule() = default;
    virtual std::string id() const = 0;
    virtual std::vector<std::string> operations() const = 0;
    virtual const IProtocolRequestAdapter& requestAdapter(
        const std::string& operation) const = 0;
    virtual const IProtocolResponseSinkFactory& responseSinkFactory(
        const std::string& operation) const = 0;
    virtual const ICapabilityMapper& capabilityMapper() const = 0;
};
```

## 2. Protocol Registry

```cpp
class ProtocolRegistry {
public:
    void registerModule(std::shared_ptr<IProtocolModule> module);
    const IProtocolModule* find(const std::string& protocolId) const;
    const IProtocolModule* findRoute(
        const std::string& method,
        const std::string& path) const;
};
```

Registry 负责：

- 协议模块注册；
- 路由和操作发现；
- 重复注册校验；
- 启动时配置检查；
- 协议能力查询。

`dispatch(raw)` 是生产入口。它按 method/path 找到路由，确定 operation，执行
Request Adapter，并在同一结果中携带 Sink 工厂和该 operation 的协议能力。这样
后台 Use Case 不需要再次按统一枚举选择协议操作，也不会把路由字符串散落到核心。

```cpp
struct ProtocolDispatchResult {
    const IProtocolModule* module;
    const IProtocolResponseSinkFactory* responseSinkFactory;
    std::string operation;
    GenerationCapabilities protocolCapabilities;
    AdapterResult adaptation;
};
```

新增协议不应要求修改 `GenerationService`。

## 3. 扩展字段

协议特有字段可以临时放入：

```cpp
struct ProtocolExtensions {
    Json::Value request;
    Json::Value response;
};
```

使用限制：

1. Adapter 可以写入；
2. 对应 Sink 可以读取；
3. 核心 Pipeline 不读取；
4. 必须进行敏感数据脱敏；
5. 稳定且跨协议的语义应最终提升为强类型字段。

## 4. 协议版本

协议模块应显式声明版本或方言：

```cpp
struct ProtocolDescriptor {
    std::string id;
    std::string version;
    std::vector<std::string> operations;
};
```

这样未来同一协议存在多个版本时，可以通过 Adapter 选择，而不污染统一模型。

## 5. 新增协议的理想改动集

```text
+ protocol/<id>/<Id>RequestAdapter.*
+ protocol/<id>/<Id>JsonSink.*
+ protocol/<id>/<Id>ToolAdapter.*
+ protocol/<id>/<Id>CapabilityMapper.*
+ protocol/<id>/<Id>ErrorFormatter.*
+ protocol/<id>/<Id>ProtocolModule.*
+ controller/<Id>Controller.*
+ tests/protocol/<id>/*
~ ProtocolRegistry wiring
~ configuration
```

以下模块原则上不变：

```text
GenerationService
GenerationPipeline
IChatProvider
ContinuityResolver
AccountSelectionPolicy
```

模拟协议测试和正式 `anthropic-messages` 模块都只通过模块、路由、Adapter、Sink
Factory、composition root 和测试完成接入；`GenerationService`、
`GenerationPipeline`、`ToolCallBridge`、`IChatProvider` 均未增加 Claude 分支。

当前生产 Registry 包含：

| 协议 ID | 版本 | operation | 路由 |
|---|---|---|---|
| `openai-compatible` | `v1` | `chat.completions`、`responses.create` | `/v1/chat/completions`、`/v1/responses` 及 provider 前缀路由 |
| `anthropic-messages` | `2023-06-01` | `messages.create` | `/v1/messages` 及 provider 前缀路由 |
