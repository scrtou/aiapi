# contracts 目录说明

## 职责

`contracts` 只承载“稳定的数据与接口契约”，避免业务实现细节耦合进来。

## 当前文件

- `GenerationRequest.h`：生成请求统一模型
- `GenerationSession.h`：生成执行聚合
- `GenerationEvent.h`：统一事件定义
- `IResponseSink.h`：输出通道抽象接口

`GenerationRequest` 只提供 canonical 内容块、强类型工具定义/选择、协议/模型/Provider/effective
能力及协议扩展区。扩展区只允许由协议适配器写入、对应边界读取，核心 Pipeline 不解释
其中的厂商字段。

协议原始工具树只能写入 `protocolExtensions`。Session 边界根据强类型定义生成执行编码，
核心调用方不得同时提交 JSON 工具定义和强类型工具定义。

`Message` 只保存 `blocks`。核心读取通过 `getTextContent()`、`hasToolUses()` 和
`toolResultCallId()` 访问 canonical 内容。流式工具 Sink 可选择消费带序号的
`ToolCallStarted`、`ToolArgumentsDelta`、`ToolCallDone` 生命周期。

## 设计约束

- 尽量保持“轻依赖”，避免依赖 `continuity/` 与 `tooling/` 的实现细节。
- 契约变更必须保持所有权边界明确，尤其是 Controller、协议工厂与 Sink 之间的接口。
