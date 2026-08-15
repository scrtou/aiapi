# ADR-10 领域模型与 JSON/DB codec 边界

| 项 | 值 |
|---|---|
| 状态 | 已接受，已实施（P3-W4、P8-W1） |
| 当前版本 | v2.0 |
| 关联 | ADR-01、ADR-02、ADR-05 |

## 决策

domain model 只表达业务值和不变量，不提供 `fromJson/toJson`，不包含 JsonCpp、Drogon、OpenSSL 或
SQL 类型。HTTP JSON、上游 JSON 和 DB snapshot 在边缘拥有各自的 codec：

```text
transport request copy -> application request adapter -> domain/application command
provider codec         -> ProviderRequest/ProviderResponse
store codec            -> domain value/snapshot
```

协议未知字段留在 edge-owned JSON document；JSON 不可作为跨 domain 边界的通用逃生舱。公开响应的
snake/camel 命名只由 transport 决定；凭据、Cookie、密码不进入公开视图。

## 实施结果

- `src/domain/` 的 JsonCpp/Drogon/SQL/OpenSSL 引用为 0；
- `ProviderResult` compatibility model 和 `ProviderResultCodec` 已删除；成功输出统一为 JSON-free
  `ProviderResponse { text, Usage, ToolCall[], ProviderMetadata }`，失败统一为 `platform::Error`；
- Session、Account、Channel、Workspace 和 Error domain data 均为 C++ 字段；DB/HTTP JSON materialization
  留在它们的 application/infrastructure/transport edge codec；
- 为保持公开协议兼容，application 的 `RequestAdapters` 是受限 codec entry：它只接收 Controller 已复制的
  `Json::Value` 和 header value object，立即构造 `GenerationRequest`，不把 JsonCpp 传播给 domain/port；
- `AiApiUseCase` 的 Controller port 使用序列化 request/response strings 和值对象，不暴露 JsonCpp 或
  Drogon 类型。

## 验收

architecture/layer/ADR-09 gates 对 domain 的第三方 include 和 application HTTP framework leak 均为 0；
JSON codec、Provider fixture、RequestAdapters 和 HTTP/SSE 输出契约测试保护现有公开行为。任何新 domain
model 若需要 JSON/DB 映射，必须在对应边缘添加 codec 与往返/契约测试，不能把方法放回 domain 类型。
