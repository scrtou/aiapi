# 可扩展 AI 接口架构设计

## 文档目的

本目录描述 aiapi 的目标架构：建立一个**协议无关、可扩展、可测试的统一生成内核**，使系统能够支持当前接口，并在未来以较低成本增加一个或多个新的 AI 接口协议。

这里的重点不是为某个具体厂商复制一套实现，而是明确哪些内容属于统一生成能力，哪些内容属于协议适配能力，以及新增协议时应遵守的边界。

## 当前状态（2026-08-15）

OpenAI-compatible 协议链路已经完成架构收敛，Claude/Anthropic Messages API 已作为
第二个正式协议接入：

- 生产请求通过 `ProtocolRegistry::dispatch()` 完成路由、操作、Request Adapter、Sink 工厂和协议能力绑定；
- Controller 只提供 JSON/SSE IO 回调，具体 Sink 由协议模块工厂创建；
- `Message` 仅保存 `ContentBlock`，工具定义和工具选择也只有强类型内部表示；协议 wire shape 只在 Adapter 和 Sink 边界转换；
- 能力按协议、模型声明和 Provider 能力求交集，并对图片、流式、工具和并行工具给出明确错误或降级；
- 工具流式生命周期支持 `ToolCallStarted → ToolArgumentsDelta* → ToolCallDone`，Sink 按调用 ID、序号和终态去重；
- 模拟协议已完成 Registry/Dispatcher 边界验收，未修改 `GenerationService`、`GenerationPipeline` 或 Provider 接口；
- `anthropic-messages` 模块实现 `messages.create` 请求适配、非流式 JSON、Anthropic SSE、工具调用、错误格式和能力映射；
- 已注册 `/v1/messages`、`/chaynsapi/v1/messages` 和 `/retoolapi/v1/messages`，并使用协议专用限流错误格式；
- Claude 接入没有修改 `GenerationService`、`GenerationPipeline`、`ToolCallBridge` 或 Provider 接口。

当前 Claude 边界支持文本、base64/URL 图片、`tool_use`、`tool_result`、客户端工具、
`tool_choice`、扩展思考请求字段、同步响应和流式响应。服务端工具、document/search
内容块等无法映射到统一模型的类型会返回明确的不支持错误。

## 核心目标

1. 保留现有接口行为，避免已有客户端回归。
2. 将 HTTP/JSON/SSE 协议差异隔离在边界层。
3. 让核心生成流程只处理统一的生成语义。
4. 让 Provider 与客户端协议完全解耦。
5. 支持文本、图片、工具调用、流式输出、连续会话和错误处理。
6. 新增协议时，原则上只新增适配模块、路由和测试，不复制核心流程。

## 文档

- [01-目标与设计原则](./01-goals-and-principles.md)
- [02-总体架构与分层](./02-architecture-and-layers.md)
- [03-统一领域模型](./03-unified-domain-model.md)
- [04-协议扩展机制](./04-protocol-extension.md)
- [05-请求响应与流式边界](./05-request-response-streaming.md)
- [06-实施计划](./06-implementation-plan.md)
- [07-测试与验收](./07-testing-and-acceptance.md)
- [08-新增协议操作手册](./08-adding-a-new-protocol.md)

## 一句话原则

> 协议负责翻译和绑定 Sink，核心负责生成，Provider 负责连接上游，Sink 负责输出。
