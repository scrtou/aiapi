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
<Protocol>JsonSink.*
<Protocol>SseSink.*
<Protocol>ToolAdapter.*
<Protocol>CapabilityMapper.*
<Protocol>ErrorFormatter.*
<Protocol>Module.*
```

JSON 和 SSE Sink 属于同一协议目录，由该协议的 `IProtocolResponseSinkFactory`
按 operation 和 IO binding 创建。组合根只注册模块和路由，不持有协议具体 Sink。
当前目录形状如下：

```text
protocol/openai/
  OpenAiProtocolModule.*
  OpenAiRequestAdapter.*
  OpenAiChatJsonSink.*
  OpenAiChatSseSink.*
  OpenAiResponsesJsonSink.*
  OpenAiResponsesSseSink.*
  OpenAiErrorFormatter.*
protocol/claude/
  ClaudeProtocolModule.*
  ClaudeRequestAdapter.*
  ClaudeJsonSink.*
  ClaudeSseSink.*
  ClaudeErrorFormatter.*
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

## 8. Claude 阶段的实施结果

Claude 协议已按本手册作为实际第二协议实现，覆盖消息内容块、工具输入/结果、
非流式 JSON、SSE 事件、错误和能力映射。主要改动范围为：

```text
protocol/claude/
路由与 composition root 注册
协议契约测试
HTTP 路由和 Claude 限流错误格式
```

不修改 `GenerationService`、`GenerationPipeline`、`ToolCallBridge` 或 Provider
接口。生产 Registry 已注册 `anthropic-messages@2023-06-01` 的
`messages.create` operation。

当前请求映射：

- `model`、`max_tokens`、`stream`、`temperature`、`top_p`；
- 顶层 `system` 以及 `messages[]` 中的 `role: "system"` 均映射到统一请求的
  `systemPrompt`；两者都只接受字符串或 text blocks，system 指令不会进入会话历史或
  当前用户输入；
- user/assistant 文本、图片、`tool_use`、`tool_result`、thinking 历史块；
- `tools[].input_schema`、`tool_choice`、并行工具开关；
- `thinking`、`stop_sequences`、`top_k`、`service_tier` 等边界扩展字段。

当前响应映射：

- Anthropic `message` JSON；
- `message_start/content_block_*/message_delta/message_stop` SSE；
- `tool_use` 与 `input_json_delta`；
- Anthropic `error` JSON/SSE 和 stop reason。

无法映射到统一模型的 server tools、document/search 等内容块会明确拒绝，不静默
降级。

Claude Code 实际请求验收（`/tmp/claude-request.json`）还确认了以下边界：请求包含
`GPT-5.6 Luna`、33 个工具和一个数组形式的 `messages[].role=system`；适配结果保留
33 个强类型工具定义，将 system 文本合并到 `systemPrompt`，并从 `currentInput` 中
排除该文本。
