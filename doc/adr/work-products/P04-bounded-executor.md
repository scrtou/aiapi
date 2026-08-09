# P4-W1 · 有界 executor 与四态队列

| 项 | 值 |
|---|---|
| 状态 | DOING（C1/C2 完成，进行中：C3） |
| 前置 | P3-W4 domain JsonCpp 归零；P1-W5 SIGTERM/积压/断连 harness |
| 目标 | `BackgroundTaskQueue` 三 bool → 四态状态机；无界队列 → 容量上限；`bool` → `EnqueueResult`；所有调用点处理失败 |
| 计划依据 | migration-plan 阶段 4.2 与 4.5 阶段边界 |

## 1. 开工现状（实测）

`src/utils/BackgroundTaskQueue.h`，header-only 进程级单例（Meyers singleton）：

| 维度 | 现状 | 问题 |
|---|---|---|
| 状态表示 | `started_` / `stopping_` / `shutdownCalled_` 三个独立 bool | 8 种组合中只有 4 种合法，非法组合靠注释约束而非类型 |
| 队列容量 | `std::queue<NamedTask>` 无上限 | 上游过载时无界增长，无背压信号 |
| 入队返回 | `bool` | 无法区分 QueueFull 与 ShuttingDown，调用方无从选择 503 还是 429 |
| 自动启动 | `enqueue` 内隐式 spawn 8 线程 | 启动时机不受 composition root 控制，P4-W2 AppContext 需先消除 |
| 完成通知 | 无 | AppContext 无法 `wait_until` 到 drain 完成，只能阻塞 join |
| 递归入队 | Draining 期间未拒绝 | 任务内再 enqueue 可无限延长 drain |

N2 修复（停机判断前置于自动启动分支、`shutdownCalled_` 不可逆）必须原样保留，
本工作项只扩展状态机，不得回退该不变量。

## 2. 调用点 inventory（23 处生产调用，实测）

| 模块 | 文件 | 处数 | 当前返回值处理 |
|---|---|---:|---|
| composition root | `main.cc` | 1 | 丢弃 |
| transport | `controllers/AiApiController.cc` | 4 | 2 处检查（非流式 chat/responses），2 处丢弃（两条流式路径） |
| transport | `controllers/AccountController.cc` | 5 | 全部丢弃 |
| transport | `controllers/ChannelController.cc` | 2 | 全部丢弃 |
| application | `retoolWorkspace/RetoolWorkspaceService.cpp` | 1 | 丢弃 |
| infrastructure | `dbManager/session/SessionDbManager.cpp` | 5 | 全部丢弃 |
| infrastructure | `dbManager/chaynsThread/chaynsThreadDbManager.cpp` | 5 | 全部丢弃 |

合计 23 处，其中 **21 处丢弃返回值**。这正是 4.5 所说的“bool enqueue + 忽略返回值”
丢任务行为——若先迁 Provider 再改队列，该行为会被固化进新接口。

### 2.1 丢弃的真实后果分级

- **静默丢持久化**（`dbManager` 10 处）：停机窗口内 session/responseIndex/chaynsThread
  的 upsert/delete 被拒后无任何记录，重启后状态与内存不一致；
- **静默丢用户可见工作**（`AiApiController` 2 处流式）：客户端已收到 200/SSE 头，
  但生成任务从未执行，表现为永久挂起直到超时；
- **静默丢后台维护**（Account/Channel/Retool 8 处）：配额校验、自动注册、workspace
  provision 被跳过，无告警。

三类后果的处理策略必须不同，不能统一 `LOG_WARN` 了事。

## 3. 目标契约

```text
enum class EnqueueResult { Accepted, QueueFull, ShuttingDown, Stopped };

state: Fresh --start()--> Running --shutdown()--> Draining --(queue empty)--> Stopped
```

- `Fresh`：未启动。入队返回 `Stopped`（不再隐式 spawn，由 composition root 显式 `start`）；
- `Running`：入队成功 `Accepted`；队列达 `capacity` 返回 `QueueFull`；
- `Draining`：拒绝一切新任务，返回 `ShuttingDown`；已入队任务继续执行；
- `Stopped`：终态，不可逆，返回 `Stopped`。

状态迁移单向，无任何回边——这是 N2 不可复活不变量的类型化表达。

## 4. 分片计划

| 切片 | 内容 | 状态 |
|---:|---|---|
| C1 | 引入 `EnqueueResult` 与四态 `State`，`enqueue` 返回新类型，保留 `bool` 兼容 shim | DONE |
| C2 | 加入 capacity 上限与 `QueueFull`；Draining 拒绝递归入队 | DONE |
| C3 | 加入完成通知（`waitUntilIdle(deadline)`），供 P4-W2 AppContext 使用 | DOING |
| C4 | transport 层 11 处调用点处理结果：503 + Retry-After | TODO |
| C5 | infrastructure 层 10 处调用点处理结果：失败计入 metrics，不静默 | TODO |
| C6 | `main.cc` 与 RetoolWorkspaceService 2 处；移除隐式自动启动 | TODO |
| C7 | 删除 `bool` 兼容 shim；契约测试补齐；文档与 workbook 收口 | TODO |

### 分片进度证据

- C1/C2：三 bool 已删除，状态由单一 `State` 表示；`kDefaultCapacity = 1024` 背压上限生效；
  `createForTesting()` 使每个用例持有独立实例（状态机不可逆，共享单例会污染后续用例）。
  TSan 全量 276 用例 / 1417 断言 PASS，data race 为 0；停机专项与信号夹具各 5/5；
  P1-W5 既有 harness 无回归。回归入口固定为 `tools/run-tsan.sh`（裸跑二进制缺 `TSAN_OPTIONS`
  会把第三方已知告警升级为非零退出码，产生假阳性）。
- C3～C7：未完成。23 处生产调用点仍全部经由 `enqueueLegacy` 兼容 shim，
  退出门禁「23/23 调用点显式处理 `EnqueueResult`」尚未达成。

## 5. 退出门禁

- 三 bool 已删除，状态由单一 `State` 表示，非法组合不可构造；
- 23/23 调用点显式处理 `EnqueueResult`，无丢弃；静态门禁检查 `[[nodiscard]]`；
- QueueFull 背压、Draining 拒绝递归、Stopped 不可复活、drain 不丢任务四类契约测试通过；
- P1-W5 既有 SIGTERM/积压/断连 harness 全部仍通过，行为无回归；
- normal/coverage/ASan 全量 PASS；六项架构门禁 PASS；legacy ceiling 不回升。

## 6. 回滚

C1–C7 独立提交。C4/C5 若引发行为回归，可单独回滚该层调用点而保留队列实现
（兼容 shim 在 C7 前一直存在，正是为此保留）。
