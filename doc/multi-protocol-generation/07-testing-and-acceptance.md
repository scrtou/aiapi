# 07. 测试与验收

## 1. 统一语义测试

不经过 HTTP，直接验证：

- 文本消息；
- 图片消息；
- 工具调用；
- 工具结果；
- 多轮会话；
- 流式事件顺序；
- 错误和重试语义。

## 2. 协议适配测试

每个协议必须测试：

```text
协议请求 JSON → GenerationRequest
GenerationEvent → 协议非流式 JSON
GenerationEvent → 协议流式事件
GenerationError → 协议错误 JSON/SSE
```

## 3. 核心集成测试

验证：

- Adapter 能被 Registry 找到；
- Service 能调用 Provider；
- Provider 不读取协议字段；
- Sink 能消费统一事件；
- Controller 不复制生成流程。

## 4. 扩展性验收

使用一个模拟的第三协议进行验证。模拟协议至少实现：

- 一个普通请求；
- 一个流式请求；
- 一个工具调用；
- 一个错误响应。

验收要求：

- 不修改 `GenerationService`；
- 不修改 `GenerationPipeline`；
- 不修改 `IChatProvider`；
- 只新增协议模块、路由和测试；
- 原有协议测试全部通过。

当前已落地 `SimulatedProtocolModule` 和正式 Claude 模块：两者都通过
`ProtocolRegistry::dispatch()` 返回自有 operation 和 Sink factory。Claude 接入没有
修改核心生成服务、工具桥或 Provider。

本轮新增的定向验收包括：

- `ProtocolRegistry_SimulatedProtocolNeedsOnlyBoundaryChanges`；
- `GenerationCapabilities_IntersectionIsExplicit`；
- `AiApiUseCase_RejectsCapabilityMissingFromDeclaredModel`；
- `Sinks_ChatSse_ConsumesIncrementalToolLifecycle`；
- `Sinks_ResponsesSse_StreamsIncrementalToolLifecycleOnce`；
- `Sinks_ResponsesSse_ParallelToolsKeepOutputIndexOrder`；
- `ClaudeProtocol_DefaultRoutesAndCapabilities`；
- `ClaudeRequestAdapter_MapsCanonicalMessagesAndTools`；
- `ClaudeRequestAdapter_SplitsParallelToolResultsInHistory`；
- `ClaudeJsonSink_EncodesMessageAndToolUse`；
- `ClaudeSseSink_EncodesAnthropicEventSequenceAndDeduplicatesArguments`；
- `ClaudeJsonSink_FormatsAnthropicError`；
- `ClaudeController_InvalidJsonUsesAnthropicErrorShape`。

## 5. 稳定性测试

- 上游超时；
- Provider 错误；
- 客户端断开；
- 大请求体；
- 长上下文；
- 多轮工具调用；
- 并行工具调用；
- 重试和取消；
- 重复关闭和资源清理。

## 6. 禁止回归

- 核心层出现新的协议名称判断；
- Provider 返回协议专用 JSON；
- Sink 直接调用 Provider；
- 工具名称替代工具 ID；
- 已发出的流式事件重复发送；
- 不支持能力被静默伪造。

SSE 工具测试还覆盖了重复 started、重复 sequence delta 和重复 terminal done；完整
测试套件仍需在 Provider 真实流式接口接入后补充跨重试端到端场景。
