# 02. 总体架构与分层

## 1. 总体结构

```text
┌────────────────────────────────────────────────────────┐
│ Transport Layer                                       │
│ Controller / Route / Auth / Rate Limit / HTTP         │
└─────────────────────────┬──────────────────────────────┘
                          ▼
┌────────────────────────────────────────────────────────┐
│ Protocol Boundary                                     │
│ Registry Dispatcher / Request Adapter / Sink Factory   │
└─────────────────────────┬──────────────────────────────┘
                          ▼
┌────────────────────────────────────────────────────────┐
│ Unified Generation Contract                            │
│ GenerationRequest / GenerationEvent / Result / Error   │
└─────────────────────────┬──────────────────────────────┘
                          ▼
┌────────────────────────────────────────────────────────┐
│ Application Generation Core                           │
│ Service / Pipeline / Continuity / Tool Bridge         │
└─────────────────────────┬──────────────────────────────┘
                          ▼
┌────────────────────────────────────────────────────────┐
│ Provider Port                                         │
│ IChatProvider / Provider Capabilities                 │
└─────────────────────────┬──────────────────────────────┘
                          ▼
┌────────────────────────────────────────────────────────┐
│ Upstream Provider Implementations                     │
└────────────────────────────────────────────────────────┘
```

## 2. Transport 层

职责：

- 注册 HTTP 路由；
- 读取请求体；
- 认证和限流；
- 建立普通响应或 SSE 响应；
- 处理客户端断开；
- 将请求交给统一 Use Case。

禁止：

- 直接调用具体 Provider；
- 拼接模型响应；
- 实现工具调用逻辑；
- 在 Controller 中复制生成流程。

## 3. Protocol Boundary 层

每个协议模块负责：

- 解析请求；
- 校验协议字段；
- 映射到统一请求；
- 将统一事件编码为目标协议；
- 将统一错误编码为目标协议。

生产请求不是由 Controller 选择具体 Sink。Controller 只复制路由、请求头和
JSON/SSE IO 回调；`ProtocolRegistry::dispatch()` 根据 method/path 返回已解析的操作、Adapter 结果、
协议能力和对应 `IProtocolResponseSinkFactory`。Use Case 将统一请求排队后直接调用
该工厂创建 Sink。

建议抽象：

```cpp
class IProtocolRequestAdapter {
public:
    virtual ~IProtocolRequestAdapter() = default;
    virtual AdapterResult adapt(const RawProtocolRequest&) const = 0;
};

class IProtocolResponseSinkFactory {
public:
    virtual ~IProtocolResponseSinkFactory() = default;
    virtual std::shared_ptr<IResponseSink> create(
        const ResponseContext&) const = 0;
};
```

当前 OpenAI Sink 的物理实现仍在 `transport/controllers/sinks/`，这是为了复用
Drogon IO 类型；其生产构造已经只存在于 composition root 注入的协议工厂中。未来
可以把实现文件移动到协议目录，而无需改变核心接口。

## 4. Unified Contract 层

这是系统最重要的稳定边界，不能依赖 Drogon、OpenAI、Anthropic 或任何具体 Provider。

包含：

- 消息和内容块；
- 工具定义和工具调用；
- 统一生成请求；
- 统一生成事件；
- 统一错误和用量；
- 协议无关的能力描述。

## 5. Application 层

职责：

- 选择 Provider；
- 执行连续会话解析；
- 调用 Provider；
- 管理重试和超时；
- 调度工具调用；
- 分发统一事件；
- 处理请求生命周期。

Application 层不应知道客户端使用哪一种 JSON 或 SSE 格式。

`AiApiUseCase` 只消费 Dispatcher 的适配结果和能力结果。操作解析、Sink 选择和
协议能力均由 Registry 的注册路由决定；响应持久化由 Sink 的持久化能力决定。

## 6. Provider 层

Provider 接收统一请求，返回统一事件或统一结果。Provider 不应返回 OpenAI JSON、Anthropic JSON 或其他客户端格式。

## 7. 推荐目录

```text
src/application/generation/contracts/
src/application/generation/core/
src/application/generation/tooling/
src/application/generation/protocol/
  common/
  openai/
  <future-protocol>/
src/application/generation/sinks/
src/domain/port/
src/transport/controllers/
tests/protocol/
```

本轮不创建 `anthropic/` 目录；Claude 是后续独立协议阶段的工作项。
