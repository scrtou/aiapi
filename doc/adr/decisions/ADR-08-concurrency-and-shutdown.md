# ADR-08 并发、背压与停机契约

| 项 | 值 |
|---|---|
| 状态 | 已接受，已实施（P4-W3/P8-W1） |
| 当前版本 | v4.0 |

## 运行模型

1. Drogon event-loop 只做解析、轻量校验和响应调度；禁止同步 HTTP、同步 DB、`sleep_for` 和长计算。
2. 当前生成 Pipeline 保持同步签名，但整体运行在受控 worker 队列。
3. worker 内允许有界阻塞，但必须同时具备 deadline、取消检查和背压。
4. 周期任务使用可停止 timer 或可 join worker；禁止 `detach + while(true)`。
5. 跨线程回调通过 loop 调度返回 Controller/Sink，worker 不直接操作 loop-affine 对象。

## BackgroundTaskQueue 状态机

```text
NotStarted -> Running -> Draining -> Stopped
```

- `Draining/Stopped` 不可回到 Running；
- Draining 拒绝新任务，已接受任务按截止时间排空；
- enqueue 返回 `Accepted / QueueFull / ShuttingDown / Stopped`；
- 队列必须有容量或并发上限，不能无限增长；
- 调用方必须处理 enqueue 失败；持久化任务至少记录结构化 ERROR 和任务名；
- worker 边界捕获异常并记录任务上下文。

## deadline 与取消

同一请求使用一个绝对 deadline，经 `ProviderCallContext` 传播：

- HTTP timeout 不得超过剩余 deadline；
- 轮询/重试等待使用可取消的 `condition_variable::wait_until`；
- 每次重试、每轮轮询、事件发送前检查取消；
- SSE/客户端断连触发 token；
- DB 不做“天然毫秒级”假设：关键查询使用 statement timeout，或登记为无法取消的边界。

## 停机状态机

只使用一个可配置全局宽限期 `termination_grace_seconds`，不再维护“5 秒”和“27 秒”两套口径。生产默认值由部署配置决定，测试使用缩短值。

```text
S0  停止接收新请求
S1  广播取消，停止 timer，向 Reaper/后台线程发 stop request
S2  队列进入 Draining，拒绝新任务
S3  在同一个绝对 deadline 前等待所有 worker 完成
S4  全部 join 后关闭 DB/HTTP 设施并正常退出
```

所有组件接收同一个绝对 deadline，不是每步各拿一份超时预算。

## 超时策略

C++17 的 `std::thread::join()` 没有超时。不得“join 超时后继续析构仍被线程访问的对象”。

1. worker 通过完成条件变量报告退出，AppContext `wait_until(deadline)`；
2. deadline 前全部完成后再逐一 `join()`，此时应立即返回；
3. 到期仍有线程时，记录未完成任务、线程和请求；
4. 进入明确的进程级强制退出路径，不再析构其可能访问的对象；禁止 detach 后继续正常析构；
5. 强制退出导致的未完成任务必须由指标和告警可见。

正常目标是靠取消和统一 HTTP timeout 完成 S4；强制退出只是容器 SIGKILL 前的可观测兜底。

## AppContext

AppContext 显式实现幂等 `shutdown(deadline)`。析构函数不得暗中启动复杂停机流程；main 和测试必须显式调用。

## 验收

- event-loop 阻塞检查通过；
- 队列满载有明确背压；
- shutdown 后 enqueue 不复活 worker；
- SIGTERM 集成测试覆盖空闲、HTTP 阻塞、轮询、队列积压、客户端断连；
- 正常路径无线程 detach、全部 join；
- 超时路径不析构仍被活动线程访问的对象；
- ASan/TSan 停机测试无 UAF/data race。
