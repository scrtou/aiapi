# P4-W1 · 有界 executor 与四态队列

| 项 | 值 |
|---|---|
| 状态 | DONE（C1～C7 全部完成，2026-08-09 收口） |
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
| C3 | 加入完成通知（`waitUntilIdle(deadline)`），供 P4-W2 AppContext 使用 | DONE |
| C4 | transport 层 11 处调用点处理结果：503 + Retry-After | DONE |
| C5 | infrastructure 层 10 处调用点处理结果：失败不静默（ERROR 日志带 `toString(r)` 与任务名） | DONE |
| C6 | `main.cc` 与 RetoolWorkspaceService 2 处；移除隐式自动启动 | DONE |
| C7 | 删除 `bool` 兼容 shim；契约测试补齐；文档与 workbook 收口 | DONE |

### 分片进度证据

- C1/C2：三 bool 已删除，状态由单一 `State` 表示；`kDefaultCapacity = 1024` 背压上限生效；
  `createForTesting()` 使每个用例持有独立实例（状态机不可逆，共享单例会污染后续用例）。
  TSan 全量 276 用例 / 1417 断言 PASS，data race 为 0；停机专项与信号夹具各 5/5；
  P1-W5 既有 harness 无回归。回归入口固定为 `tools/run-tsan.sh`（裸跑二进制缺 `TSAN_OPTIONS`
  会把第三方已知告警升级为非零退出码，产生假阳性）。
- C3：`waitUntilIdle(timeout)` / `waitUntilIdle()` / `runningCount()` 落地。
  空闲判定为 `tasks_.empty() && running_ == 0` 的合取——只看队列会在
  「最后一个任务已出队、仍在执行」的窗口误报空闲，让 AppContext 提前销毁依赖。
  `--running_` 与通知放在 try/catch 之外，否则一个抛异常的任务会让计数永不归零、
  等待方永久挂起。该方法只观测不推进状态机，可在 Running 下安全调用。
  新增 5 个契约用例（运行中窗口 / 异常任务 / 不推进状态机 / Fresh 立即返回 /
  与 drain 完成集合一致）。全量 281 用例 1504 断言 PASS；TSan 同样 281/1504，
  data race=0，告警 2 条为第三方已知项；停机专项与信号夹具各 5/5。
- C4：transport 11 处收敛到 `ControllerUtils.h` 的统一拒绝应答。QueueFull 是**瞬时**背压，
  回 503 且带 `Retry-After: 1`；ShuttingDown/Stopped 是**终态**，同样 503 但**刻意不带**
  `Retry-After` —— 给终态进程发重试邀请，只会把客户端引向一个正在消失的实例。
- C5：infrastructure 10 处（SessionDbManager 5 / chaynsThreadDbManager 5）全部就地判定
  `r != EnqueueResult::Accepted` 并打 `LOG_ERROR`，带 `toString(r)` 与任务名。
  这里不能沿用 transport 的 503 语义：写穿任务被拒时内存态已变更而磁盘态未跟进，
  调用方已无处返回错误，唯一正确的动作是让不一致**可观测**。
- C6：`main.cc` 显式 `start(background_task_threads)` 后再 `enqueue("init", ...)`；
  `enqueue` 内不再隐式 spawn，Fresh 态直接返回 `Stopped` 并 `LOG_ERROR`。
  隐式启动一旦保留，「忘记 start()」会被静默兜住，直到停机窗口才暴露成丢任务。
- C7：`enqueueLegacy` 兼容 shim 已删除，生产调用点命中 0；
  新增 `tools/arch/check_enqueue_result.py`，接入 CI 作为第 10 个门禁 step（第 8 个脚本）。
  全量 282 用例 / 1513 断言 PASS（normal / coverage / ASan 三构建一致），
  ASan+UBSan 零报告，全部 10 个门禁 step rc=0，P1-W5 停机 harness 2/2 无回归。

## 5. 退出门禁

- 三 bool 已删除，状态由单一 `State` 表示，非法组合不可构造；
- 23/23 调用点显式处理 `EnqueueResult`，无丢弃；静态门禁检查 `[[nodiscard]]`；
- QueueFull 背压、Draining 拒绝递归、Stopped 不可复活、drain 不丢任务四类契约测试通过；
- P1-W5 既有 SIGTERM/积压/断连 harness 全部仍通过，行为无回归；
- normal/coverage/ASan 全量 PASS；全部 10 个门禁 step PASS；legacy ceiling 不回升。

## 6. 回滚

C1–C7 独立提交。C4/C5 若引发行为回归，可单独回滚该层调用点而保留队列实现
（兼容 shim 在 C7 前一直存在，正是为此保留）。

C7 之后 shim 已删除，回滚粒度随之变粗：再要回退单层调用点，只能连同
`1e06204` 一起 revert。这是删 shim 的既定代价，不是疏漏。

---

## 7. 续篇（2026-08-10）· ErrorStats 落库端口与启动接线门禁

本节记录 P4-W1 收口之后、P4-W2 正式开工之前补做的一条依赖倒置。它不改队列语义，
但同属「把构造与接线权收回 composition root」这条主线，故并入本产物而非新开工作项。

### 7.1 改动

| 项 | 内容 |
|---|---|
| 新增端口 | `src/domain/port/IErrorStatsSink.h` |
| 实现方 | `metrics::ErrorStatsDbManager`（infrastructure，保持单例） |
| 消费方 | `metrics::ErrorStatsService`，改持 `IErrorStatsSink*`，不再自取具体类型 |
| 接线点 | `src/main.cc` 第 268 行 `setSink(...)`，早于第 271 行 `init(statsConfig)` |
| 门禁 | `check_startup_wiring.py` 新增第 6 条 `REQUIRED` 规则 |
| 测试 | `src/test/test_error_stats_sink_port.cpp` |

注入必须早于 `init()`，因为 `init()` 会立刻调 `sink->init()` 建表并拉起后台 flush 线程；
晚一行，建表就落到另一个实例上。

### 7.2 门禁规则表升为四元组

原 `REQUIRED` 是三元组，FAIL 文案对所有规则硬编码「运行期将退化为 Null 实现」。
该描述对本条规则**不成立**：`ErrorStatsService::init()` 内有 `if (!dbManager_)` 回退分支，
漏注入既不崩溃也不走 Null，而是**静默绕开端口回落到 `ErrorStatsDbManager` 具体单例** ——
功能表面正常，倒置在运行期等于没做，排障者却会被文案引去找 Null 实现。

故 `REQUIRED` / `REQUIRED_STATIC` 均加第四字段 `impact`，FAIL 时按规则打印各自后果。
现有 7 条规则的后果分三类：崩溃（`AccountManager.setStore`）、
静默功能缺失（`setChannelStore`、`setRetoolProvisionClock`、`HealthController::setDbProbe`）、
静默绕开端口（`metrics::ErrorStatsService.setSink`）。

### 7.3 变异验证（三条探针，全部实测）

| 变异 | 期望 | 实测 |
|---|---|---|
| 删除 `main.cc` 中 `setSink` 注入 | rc=4 | rc=4，FAIL 打印「静默绕开端口……回落到具体单例」 |
| 将 `setSink` 挪到 `init()` 之后 | rc=4 | rc=4，FAIL 打印「注入在第 270 行，晚于 init 的第 269 行」 |
| 回滚 | rc=0 | rc=0，7 条规则全 OK |

第二条探针是必跑的：四元组改造动了 FAIL 文案的 `%` 参数个数，顺序分支若未被触发，
格式化参数不匹配会直到真正踩线那天才暴露 —— 那正是门禁最不该出错的时刻。

### 7.4 过程中的两次自我纠错

1. **命名空间误判**：首次写注入语句时假定 `ErrorStatsDbManager` 在全局命名空间，
   实际位于 `namespace metrics`，编译直接失败。`ErrorStatsService.cpp` 内可裸写，
   是因为该文件本身就在 `metrics` 命名空间中。未跑编译就报「已接线」，
   交付的会是一个编不过的 composition root。
2. **文档脚本换行错误**：改 `tools/arch/README.md` 的脚本在字符串字面量里嵌了真实换行，
   `SyntaxError` 使脚本在任何写操作前中止，文件未被污染（`git diff` 已确认）。
   写入仍走 P4-W1 定下的「内存 encode → 临时文件 → `os.replace`」原子路径。

### 7.5 验证状态

- 全量 283 用例 PASS（10.63s / 11.10s 两次复跑一致）；
- 6 个架构门禁脚本 rc=0，`check_startup_wiring.py` 7 条规则全 OK；
- 文档同步：本产物、`CHANGELOG.md`、`tools/arch/README.md`。

### 7.6 回滚

端口与门禁规则可独立回退：删除 `REQUIRED` 第 6 条即解除门禁约束；
`ErrorStatsService::init()` 的 `if (!dbManager_)` 回退分支仍在，去掉 `main.cc` 注入后
运行期会回落到具体单例，行为等价于倒置前。四元组改造是纯文案层，无行为影响。
