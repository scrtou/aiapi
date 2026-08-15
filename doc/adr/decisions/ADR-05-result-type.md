# ADR-05 跨层失败统一使用 `Result<T, Error>`

| 项 | 值 |
|---|---|
| 状态 | 已接受，部分实施（P6-W3：两家活跃 Provider 已切片） |
| 当前版本 | v3.2 |

## 决策范围

`Result<T, Error>` 用于 port、application use case、infrastructure 出口以及调用方需要处理的预期业务失败。不要求每一个私有辅助函数机械改签名。

异常不得跨越 infrastructure/transport 适配器边界；第三方异常在适配器内捕获并映射。domain 不依赖会抛第三方异常的 JSON/DB/HTTP 类型。

## Error 模型

```cpp
enum class ErrorCode {
    None = 0, // legacy/default interop only; Result failure must not use it
    BadRequest, Unauthorized, Forbidden, NotFound, Conflict,
    RateLimited, Timeout, ProviderError, Internal, Cancelled
};

struct Error {
    ErrorCode code;
    std::string message;       // 可安全返回给调用方
    std::string providerCode;  // 上游原始分类
    int upstreamHttpStatus = 0; // 上游实际状态（不替代语义 HTTP 映射）
    std::string detail;        // 仅日志，不返回客户端
};
```

- `ErrorEvent` 是观测事件，不与返回值 `Error` 合并；
- Provider 内部可保留更细错误类型，但必须在 port 出口映射并保留原始字段；
- HTTP 状态只在 transport 映射一次。

## Result 要求

- C++17 `std::variant<success-wrapper, Error>` 实现，并提供 `Result<void>` 特化；
- 使用显式 `Ok/Err` 工厂，避免 `T == Error` 时构造歧义；
- 支持 move-only `T`；
- 类型标记 `[[nodiscard]]`，CI 将忽略 Result 的诊断视为错误；
- 错误态访问 `value()` 必须显式失败，不能返回默认值。

## 实施进度

P6-W1 已落地 `platform::Result/Error/ErrorCode`、绝对 `Deadline`、只读
`CancellationToken`、`Result<void>`、重复 generation ErrorCode alias 与 `[[nodiscard]]` 的 C++17
compile gate。`Error` 保留 `providerCode`、`upstreamHttpStatus` 和仅诊断用 `detail`；legacy
`ProviderErrorCode` 只在旧 Provider 内部保留，并有到 platform Error 的投影。

P6-W2 已完成 Chayns 的真实切片：它通过 `ProviderBase` 返回
`Result<ProviderResponse>`，不再读写 `session_st/session.response`。GenerationService 优先解析窄
`IChatProvider`，把成功 response 仅在 application 边界物化给尚存的 legacy event/session pipeline；
失败 `platform::Error` 原样进入 `generation::Error`，JSON/SSE sink 仍只通过
`platform::defaultHttpStatus` 做语义 HTTP 映射。请求 gate 持有 `CancellationSource`，Provider 仅得到
只读 token；Chayns 在账号等待、HTTP 返回、重试和 polling 边界检查取消/绝对 deadline。

P6-W3 已完成 Retool 的同一路径：它经 `ProviderBase` 返回 `Result<ProviderResponse>`，workflow/agent
的 HTTP、polling 和 sleep 均受只读 cancellation/absolute deadline 约束；只复制非敏感 workspace selector
到 `ProviderRequest::routingHints`，不再接收 session aggregate。`APIinterface`、`findProvider()`、
`registerProvider()` 与 registry legacy storage 已删除，两家活跃 Provider 都仅以 chat/model/thread 窄能力
发布。P8 已删除 `ProviderResult` compatibility codec、重复 generation `ErrorCode` 和 `Errors.h`；
成功结果由 `ProviderResponse` 承担，失败由 `platform::Error` 承担。DB/account/session 的其余公共 port
仍按垂直切片迁移，因此 ADR-05 保持“部分实施”，而不是为状态好看机械改写所有私有函数签名。

## 迁移顺序

按垂直切片迁移，不在建立 CMake target 时全项目改签名：

1. Result/Error 本体、单测和重复 `ErrorCode` 收敛；
2. chayns Provider port 出口；
3. 对应 application use case；
4. 对应 transport 错误映射；
5. [完成] Retool 重复同一路径并删除 legacy registry lane；
6. DB/account/session ports 随被修改的 use case 逐步迁移；
7. 删除旧 bool+出参、session 副作用和重复错误类型。

每个切片完成时旧路径必须删除，不建立长期双轨。

## 验收

- port/application 公共边界没有异常外泄；
- Provider 不再通过 `session.response` 表达成功或失败；
- transport 只有一个 Error → HTTP 映射入口；
- Result 被忽略时 CI 失败；
- 错误映射测试验证 providerCode/httpStatus 保真。
