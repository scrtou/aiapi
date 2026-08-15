# ADR-09 HTTP、轮询与 IO 边界

| 项 | 值 |
|---|---|
| 状态 | 已接受，已实施（P8-W1） |
| 当前版本 | v2.0 |
| 关联 | ADR-05、ADR-07、ADR-08 |

## 决策

HTTP、数据库和定时等待只属于 infrastructure/transport adapter。domain/application 接收不可变输入、
绝对 `Deadline` 和只读 `CancellationToken`；不得看到 `drogon::HttpRequestPtr`、`HttpClient`、
`DbClient` 或直接调用 `sleep_for`。

Provider 的每个阻塞边界都按 `remaining = deadline - now` 裁剪 timeout。轮询、退避和队列等待使用可
取消等待；若底层 DB/HTTP API 无法取消，必须登记 `UncancellableBoundary` 并设置最小 statement/network
timeout，不能声称“天然可取消”。

## 已实施边界

- `AiApiController` 在 transport 层读取 Drogon request，只把复制的 `Json::Value` 和
  `aiapi::RequestHeaders` 交给 `RequestAdapters`/`IAiApiUseCase`；
- `RequestAdapters::buildGenerationRequestFromChat/Responses` 的唯一输入是上述值对象，不能接收
  `HttpRequestPtr`；
- Account lifecycle 使用 framework-neutral `IAccountHttpTransport` 的 request/response DTO；
  `infrastructure/account/DrogonAccountHttpTransport` 是具体 Drogon adapter；
- `AppWiring` 读取 `custom_config` 后按值注入 Account workflow 与 `AiApiUseCase`，后台 application
  path 不回查 Drogon runtime；
- `IoLoopResponseStream` 位于 `transport/controllers/sinks/`，是 worker 回到 Drogon event loop 的唯一 adapter；
- Chayns/Retool 的同步 HTTP、轮询和 clock/sleep 均在 infrastructure，并受 deadline/cancellation
  contract 和 fixture 覆盖。

## 强制与验收

`tools/arch/check_http_io_boundary.py`：

1. 从 `AIAPI_APPLICATION_SOURCES` 跟随本地 include closure，并扫描全部 domain 文件；
2. 拒绝 Drogon/Trantor include/symbol、`HttpClient`、`DbClient`、直接 `std::this_thread::sleep_for`；
3. 确认 application 不链接 `Drogon::Drogon`，Account port 保持无框架类型，Drogon adapter 留在
   infrastructure，且 runtime-config 接线完整；
4. `--selftest` 以内存注入 Drogon include，必须返回失败。

静态 gate 与 HTTP 阻塞、轮询超时、取消、客户端断连、SIGTERM 五类行为/sanitizer 验证共同构成验收；
静态扫描不替代运行时证据。
