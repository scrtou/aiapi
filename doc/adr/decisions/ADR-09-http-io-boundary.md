# ADR-09 HTTP、轮询与 IO 边界

| 项 | 值 |
|---|---|
| 状态 | 已接受，待实施 |
| 关联 | ADR-05、ADR-07、ADR-08 |

## 决策

HTTP、数据库和定时等待只能出现在 infrastructure/transport adapter。domain/application 接收
不可变请求、绝对 `Deadline` 和 `CancellationToken`，不得看到 `drogon::HttpRequestPtr`、
`HttpClient`、`DbClient` 或直接调用 `sleep_for`。

Provider 的每个阻塞边界都计算 `remaining = deadline - now`；HTTP timeout 不得超过 remaining。
轮询、退避和队列等待使用可取消的 `wait_until`。如果底层 DB/HTTP API 无法取消，必须登记为
`UncancellableBoundary` 并设置最小 statement/network timeout，不能声称“支持取消”。

## 责任划分

- `IUpstreamHttpClient`：只负责请求/响应和 transport error；不做 session、账号选择或重试。
- `RetryPolicy`/`PollingPolicy`：纯策略，输入上次结果、时间和取消状态，输出下一动作。
- `ProviderBase`：只做 NVI 公共边界（见 ADR-07），不规定 workflow、SSE 或 polling 流程。
- `IoLoopResponseStream`：唯一允许 worker 回到 Drogon loop 的 adapter；sink 不得直接触碰 loop-affine 对象。

## 验收

静态扫描中 application/domain 的 Drogon/DB include 和同步 HTTP 为 0；五类测试（HTTP 阻塞、轮询
超时、取消、客户端断连、停机）验证 deadline 不被重置。
