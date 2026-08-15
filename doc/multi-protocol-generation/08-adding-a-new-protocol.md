# 08. 新增协议操作手册

## 1. 创建目录

```text
src/application/generation/protocol/<protocol-id>/
```

不要先复制 `GenerationService` 或 `GenerationPipeline`。新增协议的唯一必需核心
接点是 `IProtocolModule`、`IProtocolRequestAdapter`、`IProtocolResponseSinkFactory`
和 `ICapabilityMapper`。

建议包含：

```text
<Protocol>RequestAdapter.*
<Protocol>ResponseSink.*
<Protocol>SseSink.*
<Protocol>ToolAdapter.*
<Protocol>CapabilityMapper.*
<Protocol>ErrorFormatter.*
<Protocol>Module.*
```

## 2. 定义协议边界

首先确定：

- 路由；
- 操作类型；
- 认证和限流；
- 非流式格式；
- 流式格式；
- 工具调用格式；
- 错误格式；
- 能力声明；
- 版本或方言。

## 3. 实现请求适配器

适配器必须完成：

- 请求体校验；
- 字段转换；
- 消息和内容块转换；
- 工具定义转换；
- 工具选择转换；
- 能力校验；
- 协议扩展保存。

适配器不得调用 Provider。

## 4. 实现响应 Sink

Sink 必须完成：

- 统一文本事件编码；
- 工具调用事件编码；
- usage 和 stop reason 映射；
- 错误格式化；
- 正常关闭和异常关闭。

Sink 不得实现 Provider 重试或工具执行。

## 5. 注册模块

将模块注册到 `ProtocolRegistry`，并检查：

- ID 唯一；
- 路由唯一；
- 操作已声明；
- Adapter 和 Sink 成对存在；
- 配置已加载；
- 能力映射可用。

路由注册只在 composition root 完成。Controller 只需要设置 `protocolId`、
operation、method/path 和 IO binding；Use Case 会消费 Dispatcher 结果，不能在
Controller 或核心层按协议字符串选择 Sink。

## 6. 编写测试

至少增加：

- 请求适配单元测试；
- 非流式响应测试；
- 流式事件顺序测试；
- 工具调用测试；
- 错误映射测试；
- Registry 路由测试；
- 端到端测试；
- 原有协议回归测试。

## 7. 合并前检查

新增协议提交前必须确认：

- 没有复制 GenerationPipeline；
- 没有在核心层添加协议条件分支；
- 没有让 Provider 依赖协议类型；
- 没有把专用字段强行加入统一模型；
- 不支持的能力有明确行为；
- 连接和资源生命周期经过测试；
- 可以通过配置关闭该协议并回滚。

## 8. Claude 阶段的边界

Claude 协议是下一阶段的实际第二协议，必须按本手册独立实现其消息内容块、工具
输入/结果、非流式 JSON、SSE 事件、错误和能力映射。接入验收应证明只修改：

```text
protocol/claude/
路由与 composition root 注册
协议契约测试
```

不修改 `GenerationService`、`GenerationPipeline`、`ToolCallBridge` 或 Provider
接口。Claude 在当前提交中保持未注册状态。
