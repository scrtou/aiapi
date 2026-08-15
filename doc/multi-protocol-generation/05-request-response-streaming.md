# 05. 请求、响应与流式边界

## 1. 请求路径

```text
HTTP Request
  → Controller
  → Protocol Request Adapter
  → GenerationRequest
  → GenerationService
  → Provider
```

Adapter 完成协议校验和语义转换后，核心层不再读取原始 HTTP JSON。

## 2. 响应路径

```text
Provider
  → GenerationEvent
  → GenerationService
  → Protocol Response Sink
  → HTTP Response
```

Sink 负责协议编码，不负责生成决策。

## 3. 非流式

非流式输出可以先收集统一事件，再由 JSON Sink 生成最终响应。必须保留：

- 文本内容；
- 工具调用；
- 停止原因；
- 输入/输出用量；
- response/request ID；
- 协议需要的 metadata。

## 4. 流式

流式输出采用统一事件序列。每个协议 Sink 自己维护协议状态机：

```text
GenerationStarted
  → TextDelta* / ToolCallStarted + ToolArgumentsDelta*
  → ToolCallFinished
  → GenerationFinished
```

协议 Sink 不得透传上游原始 SSE，因为上游事件和客户端协议的事件语义可能不同。

## 5. 流式状态要求

必须处理：

- 文本增量；
- 工具参数增量；
- 多个并行工具调用；
- 不完整 JSON；
- 上游错误；
- 客户端断开；
- 超时；
- 重复关闭；
- 重试后事件重复。

工具事件使用调用 ID 和可选 `sequence` 标识。Chat/Responses SSE Sink 会拒绝重复的
started、同一 sequence 的 arguments delta 和已完成调用；终态 `ToolCallDone` 是
最终参数投影，保证不完整 JSON 也能在 done 时完成协议编码。当前 Provider Port
主要返回完整响应，因此 `GenerationResponsePipeline` 会发出一个完整参数 delta；
未来 Provider 发出多段 delta 时无需改协议 Sink。

已经发送给客户端的事件不能因为重试而再次发送。

## 6. 工具调用边界

工具调用生命周期由核心层管理：

```text
ToolCallStarted
  → ToolArgumentsDelta*
  → ToolCallFinished
  → Tool execution
  → ToolResult message
  → next generation turn
```

协议 Adapter 负责输入转换，Sink 负责输出转换，`ToolCallBridge` 只处理统一工具语义。

Responses SSE 的 native tool item 会按 started/delta/done 分别发送
`response.output_item.added`、参数 delta、参数 done 和 `response.output_item.done`，
不会因为 terminal projection 再创建第二个 output item。

Claude SSE Sink 将同一事件序列映射为：

```text
message_start
  → content_block_start
  → content_block_delta(text_delta | input_json_delta)*
  → content_block_stop
  → message_delta
  → message_stop
```

工具参数按调用 ID 和 sequence 去重；`ToolCallDone` 只关闭已有 `tool_use` 内容块，
不会重复发送参数。错误使用 `event: error` 和 Anthropic 的 `type/error` JSON 结构。

## 7. 不可无损映射

对于某协议独有且统一模型无法表达的能力，必须选择：

1. 精确映射；
2. 明确降级；
3. 明确忽略并记录；
4. 返回不支持错误。

禁止静默伪造能力或返回错误语义。
