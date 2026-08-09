# P1-W5 · SIGTERM、队列、Worker 与断连 characterization

| 项 | 值 |
|---|---|
| 状态 | DONE |
| 前置 | P1-W1～P1-W4 已完成 |
| 生产入口 | Drogon SIGTERM handler → `main` 返回 `app.run()` → `lifecycle::runApplicationShutdown` |
| 并发实现 | `BackgroundTaskQueue`、Account workers、session cleaner、`chaynsThreadReaper`、`IoLoopResponseStream` |
| 外部依赖 | harness 不连接数据库、不访问上游、不监听公开端口；全部状态为进程内合成 |
| 阶段结论 | P1 行为安全网退出门禁已通过；当前缺少全局取消、有界 drain 和 shutdown deadline，保留到 P4 修复 |

## 1. 目标与范围

P1-W5 的目标不是提前实现 P4 的理想并发模型，而是让当前停机行为可重复、可突变、可报告：

- 真实 OS `SIGTERM` 是否会让 Drogon `run()` 返回并进入 production shutdown sequence；
- 空闲 worker 的长周期等待是否可中断并 join；
- 正在运行的阻塞任务和队列积压在 `shutdown()` 下如何处理；
- shutdown 边界后的 enqueue 是否拒绝、队列是否会复活；
- 客户端断连后的流对象、late chunk 与 IO event-loop 亲和行为；
- 当前没有实现的广播取消、deadline、容量和超时兜底必须明确登记，不能把“测试通过”误写成目标架构已经完成。

测试 companion 只启动 Drogon event loop 和内存队列；Account/Session/Reaper lifecycle 测试使用空状态和链接期 DB collaborator stub。不存在真实 HTTP 请求或真实账号。

## 2. 生产停机调用图

为避免复制 `main.cc` 的停机顺序，新增了一个很窄的 production seam：

```text
ApplicationShutdownActions
  ├─ stopReaper
  ├─ stopAccountWorkers
  ├─ stopSessionCleaner
  └─ shutdownTaskQueue

lifecycle::runApplicationShutdown(actions)
  ├─ actions.stopReaper()
  ├─ actions.stopAccountWorkers()
  ├─ actions.stopSessionCleaner()
  └─ actions.shutdownTaskQueue()
```

`main.cc` 仍是 composition root，并绑定真实 owner：

```text
OS SIGTERM
  └─ Drogon TERMFunction
      └─ app().quit()
          └─ drogon::app().run() 返回
              └─ lifecycle::runApplicationShutdown
                  ├─ chaynsThreadReaper::stop()
                  ├─ AccountManager::stopBackgroundThreads()
                  ├─ chatSession::stopClearExpiredSession()
                  └─ BackgroundTaskQueue::shutdown()
```

`ApplicationShutdown` 只拥有顺序，不拥有 singleton，也没有把业务策略下沉到工具层。生产 `main` 与测试调用同一函数。

## 3. OS SIGTERM harness

新增不注册为 CTest case 的 companion：

```text
aiapi_shutdown_signal_fixture
  ├─ 启动 2 个 BackgroundTaskQueue worker
  ├─ 启动真实 Drogon app.run()
  ├─ stdout: READY
  ├─ 接收父测试进程发送的 SIGTERM
  ├─ Drogon handler 使 run() 返回
  ├─ 调用 production runApplicationShutdown
  └─ stdout: REAPER → ACCOUNTS → SESSION → QUEUE → EXIT
```

`ApplicationShutdown_IdleProcessHandlesSigtermInProductionOrder` 通过 `fork` 后立即 `exec` companion，等待 `READY`，发送真实 `SIGTERM`，在 5 秒硬测试预算内 `waitpid`，并断言 exit code 和完整顺序。超时测试会 `SIGKILL` 并回收子进程，避免 CI 残留进程。

此 harness 证明的是：

- 当前 Drogon SIGTERM handler 能使 idle event loop 返回；
- 返回后执行的是与 `main.cc` 相同的 production sequence；
- idle queue 能 join 并正常退出。

它没有启动真实 listener/数据库/provider，因此不冒充“完整部署环境 drain”。P4-W3 仍需容器级 listener、HTTP 阻塞、轮询与超时集成测试。

## 4. BackgroundTaskQueue 当前状态机

当前实现不是目标四态有界队列，精确状态为：

```text
未启动
  └─ enqueue → 懒启动固定 8 worker
  或 start(N) → 已启动

已启动 / acceptingTasks=true
  ├─ enqueue → 入无界 std::queue
  └─ shutdown
       ├─ shutdownCalled=true（不可逆）
       ├─ stopping=true / acceptingTasks=false
       ├─ 已接受任务继续 drain
       └─ join 所有 worker → started=false

已停机
  ├─ enqueue → false（fail-fast）
  └─ shutdown → 幂等 no-op
```

### 4.1 空闲与不可复活

`BackgroundTaskQueue_ShutdownIsIrreversible` 锁定：停机前任务执行、shutdown drain、pending=0、停机后拒绝、重复 shutdown 幂等，且任务池不会被迟到 enqueue 复活。

### 4.2 运行中阻塞

`BackgroundTaskQueue_ShutdownWaitsForRunningTask` 使用一个进程内 promise 阻塞唯一 worker：

```text
worker 进入任务 → shutdown() 异步调用
  ├─ 100ms 内不得返回
  └─ test release promise 后才 drain + join
```

这锁定了当前的重要缺陷：`shutdown()` 没有取消 running task、没有 deadline、没有超时后降级，也不会抢占同步 HTTP/轮询。若 task 永不返回，进程将永不完成优雅停机，只能由外部 SIGKILL。

### 4.3 积压与接收边界

`BackgroundTaskQueue_ShutdownDrainsBacklogAndRejectsLateWork` 占满两个 worker，再放入 24 个 backlog task。测试等待 `acceptingTasks()` 在互斥锁保护下变为 false，证明：

- shutdown 前已接受的 26 个任务全部执行；
- shutdown 边界后的 late work 明确返回 false；
- shutdown 返回后 pending=0；
- 没有容量、背压、优先级、逐任务 deadline 或 drop policy。

`acceptingTasks()` 是只读观察点，不改变状态机策略；它避免用 timing sleep 猜测 shutdown 是否已关闭入队边界。

### 4.4 Controller 边界差异

当前 Controller 对 enqueue=false 的处理不一致：

- Chat/Responses 非流式路径检查返回值并生成 `503 shutting_down`；
- Chat/Responses 流式 async response 内部忽略 enqueue 返回值，停机竞争时可能创建 stream 后没有 generation task/最终 close；
- 其他 account/channel/workspace/ledger fire-and-forget 路径多处也忽略返回值。

这是 P4 有界 executor 和 P5 use-case 边界的迁移输入，P1 不静默修改公开行为。

## 5. Worker interrupt/join

`ShutdownWorkers_LongWaitsAreInterruptibleAndStopsAreIdempotent` 锁定三类 idle worker：

```text
AccountManager
  ├─ tokenUpdateWorker 等待 accountListNeedUpdateCondition
  ├─ accountTypeThread 处于 1 分钟预热 backgroundSleep
  └─ stopBackgroundThreads
       ├─ backgroundStopRequested=true
       ├─ notify backgroundWakeCv + accountListNeedUpdateCondition
       └─ join 已启动 worker；重复 stop 安全

chatSession
  ├─ cleaner wait_for(3600s)
  └─ stopClearExpiredSession → flag + notify + join；重复 stop 安全

chaynsThreadReaper
  ├─ reaper wait_for(3600s)
  └─ stop → flag + notify + join；重复 stop 安全
```

三类 stop 在测试中各自小于 1 秒，不等待完整配置周期。

### 5.1 Characterization 发现并修复的丢失唤醒

首次 coverage 全量运行暴露出一个真实竞态：258 项中仅
`ShutdownWorkers_LongWaitsAreInterruptibleAndStopsAreIdempotent` 失败，Account
`accountTypeThread` 恰好等待完整的 60 秒预热期后才退出。原实现只通过 atomic
`exchange(true)` 修改等待谓词，没有持有 `backgroundWakeMutex_`，因此可能发生：

```text
worker 持锁检查 predicate=false
  → stop 在线程真正进入 wait 前设置 atomic 并 notify
  → notify 丢失
  → worker 睡满 60 秒
```

修复时没有改变公开 lifecycle：`stopBackgroundThreads()` 现在持有与
`backgroundSleep()` 相同的 mutex 修改谓词，释放锁后再 `notify_all()`；首次调用仍负责
join，后续调用仍幂等返回。这保证 worker 要么在状态修改前进入 wait 并收到通知，要么在
状态修改后取得锁并直接观察到 true。

修复后的受控稳定性证据：

```text
normal  --repeat until-fail:20: 20/20 PASS，单次 0.02～0.03 秒
coverage --repeat until-fail:20: 20/20 PASS，单次 0.13～0.17 秒
coverage 首次失败证据:       60 秒后退出，stoppedFast 断言失败
```

这项改动属于让 P1 当前 shutdown 契约可重复所必需的并发缺陷修复，不提前实现 P4 的
全局取消或 deadline。

未被这项 idle 测试证明的当前风险：

- Account token-check worker 可能正处在同步 HTTP、数据库或账号清理中；stop flag 不会中断这些调用；
- Reaper 可能正处在同步 upstream DELETE 或 `deleteSpacingMs` sleep 中；stop 只在调用/row 边界检查；
- `runApplicationShutdown` 是同步串行调用，不会让 Reaper join 与后续步骤重叠；源码中原先声称“重叠收敛”的注释已按真实行为修正；
- 任一 stop callback 抛异常会中断后续 sequence；当前没有 per-step containment 或 finally drain。

## 6. 客户端断连

`IoLoopResponseStream` 是当前 TCP stream 的窄边界：所有 send/close/destruction 都被投递到所属 event loop。

测试矩阵：

| 测试 | 当前契约 |
|---|---|
| `IoLoopResponseStream_SerializesWorkerOperations` | worker 调用的 send/close 实际在 owner loop 执行 |
| `IoLoopResponseStream_DestructorIsLoopAffine` | worker 析构也在 owner loop close/delete |
| `IoLoopResponseStream_DoesNotReenableWriteAfterQueuedDisconnect` | disconnect 先于 queued send 时不会重新启用 write event |
| `IoLoopResponseStream_SendFailureClosesAndRejectsLaterChunks` | 首次底层 send=false 后 close 一次；bridge 变为 closed；late chunk 不再触达底层 |

Chat/Responses SSE sink 在 stream callback=false 后会标为 closed 并停止后续输出。但是 sink/stream 的关闭没有连接到 `SessionExecutionGate::CancellationToken`，也没有传入 Provider HTTP/poll loop。当前因此是“停止写客户端”，不是“取消上游工作”；BackgroundTaskQueue 仍会等待 generation task 自然返回。

## 7. 测试与稳定性

P1-W5 新增/扩展 6 个测试，总 shutdown/disconnect targeted 为 10 项：

```text
ApplicationShutdown_CallsOwnershipBoundariesInOrder
ApplicationShutdown_IdleProcessHandlesSigtermInProductionOrder
BackgroundTaskQueue_ShutdownIsIrreversible
BackgroundTaskQueue_ShutdownWaitsForRunningTask
BackgroundTaskQueue_ShutdownDrainsBacklogAndRejectsLateWork
ShutdownWorkers_LongWaitsAreInterruptibleAndStopsAreIdempotent
IoLoopResponseStream_*（4 项）
```

worker idle stop 用例修复丢失唤醒后分别在 normal 与 coverage 下执行
`--repeat until-fail:20`，均为 20/20 PASS。最初尝试同时启动会先执行业务巡检的
token-check/account-count worker，会把“idle interrupt”测试与账号清理/DB 状态耦合并出现
不稳定阻塞；最终测试只启动确定处于 CV 长等待的 worker，并把同步业务调用不可取消明确
登记为当前缺口，而不是用脆弱 timing 掩盖。

最终验证：

```text
shutdown/disconnect targeted: 10/10 PASS
normal ctest:                258/258 PASS
coverage ctest:              258/258 PASS
ASan ctest:                  258/258 PASS
ASAN_OPTIONS: detect_leaks=0:halt_on_error=1
```

ASan 基础运行覆盖全量用例；关闭 LeakSanitizer 是为了不把 Drogon/全局 singleton 的进程期分配混入 AddressSanitizer 内存安全门禁。本报告不声称 LSan 已通过。

## 8. Coverage 证据

来源：`P01-runtime-coverage-report.md`。

```text
BackgroundTaskQueue.h
  lines=70/77, branches=130/230
  enqueue      exec=35, lines=17/17
  shutdown     exec=8,  lines=13/13
  workerLoop   exec=11, lines=16/23

ApplicationShutdown.cpp
  lines=17/17
  runApplicationShutdown exec=1, lines=14/14

AccountManager::stopBackgroundThreads exec=42, lines=18/18
chatSession::stopClearExpiredSession  exec=2,  lines=8/8
chaynsThreadReaper::stop              exec=3,  lines=8/8

IoLoopResponseStream::send            exec=4, lines=11/12
IoLoopResponseStream::sendInLoop      exec=3, lines=5/6
IoLoopResponseStream::closeInLoop     exec=4, lines=7/7
```

execution count 是 gcov 跨独立 CTest 进程累积值；Account singleton 的析构兜底也会调用幂等 stop，因此次数高于显式 lifecycle 测试调用数。

## 9. 受控突变

未提交的受控突变：在 production `runApplicationShutdown` 中临时交换 Account 与 Session stop 顺序。重建后：

```text
ApplicationShutdown_CallsOwnershipBoundariesInOrder
mutation_exit=8
```

测试在 `test_application_shutdown_harness.cpp:113` 失败；恢复源码、重新构建后 1/1 PASS。该证据证明顺序断言绑定 production coordinator，而不是只检查 companion 自己打印的字符串。

## 10. P1 阶段退出门禁汇总

| 生产路径 | 离线响应/入口证据 | coverage 执行证据 | 受控突变 |
|---|---|---|---|
| Chayns | 合成 thread/message/poll/model/read/delete fixture；4 output modes | `generate`/`postChatMessage` 均 exec=9 | Chayns fixture 工作项已验证 |
| Retool | 合成 workflow/agent response + fake HTTP/clock | `requestWorkflow` exec=8；`requestAgent` exec=3 | routeType 突变失败 |
| Generation | CapturingProvider 进入真实 runGuarded/transform/emit | `runGuarded`/`emitResultEvents` exec=8 | P1-W4 Account rollback 突变失败 |
| Account | store/http/clock fake；401、注册成功/失败 | add/update/delete/checkToken/autoRegister 均执行 | rollback 状态突变失败 |
| Shutdown | 真实 OS SIGTERM companion、blocked/backlog/worker/disconnect | coordinator/queue/worker/stream 函数均执行 | stop 顺序突变失败 |

阶段总门禁：

```text
fixture safety check:            PASS
architecture audit selftest:     PASS
architecture ratchet:            PASS (R1=0)
cycle/layer gate:                PASS (0 cycle)
startup wiring:                  PASS
test registration strict:        PASS (258=258)
normal ctest:                    258/258 PASS
coverage ctest:                  258/258 PASS
ASan full ctest:                 258/258 PASS
git diff --check:                PASS
```

因此阶段 1 的目标——在删除/重写前保护真实生产路径——已经满足，可以进入阶段 2。这里的“通过”不表示 P4 并发目标已实现；下面的历史缺陷正是后续 contract 测试的输入。

## 11. 遗留问题与 P4/P5 边界

- BackgroundTaskQueue 无界，且 shutdown 对 running task 无限等待；
- 没有 process-wide cancellation source，也没有把 deadline/token 传到 Provider HTTP 和 poll；
- disconnect 只关闭输出，不取消 generation；
- shutdown 没有分步 deadline、错误 containment、超时指标或强制降级策略；
- Reaper、Account、Session、queue 依然是 singleton，production action 由 `main` lambda 绑定；
- stream enqueue rejection 未在流式 Controller 中闭环；
- helper harness 没有启动真实 listener/DB/provider，P4-W3 必须补空闲、HTTP 阻塞、轮询、积压、断连五类部署级 SIGTERM 测试，并运行 TSan/LSan 范围审计。

## 12. 回滚

- `ApplicationShutdown.*` 可内联回 `main.cc`，但会失去 production 顺序的可测试入口；回滚必须同时移除 companion 和 coverage target；
- companion 只属于测试构建，可独立删除，不影响 production binary；
- queue/worker/stream 测试均可独立回滚；`acceptingTasks()` 只是只读观察点，删除它需将积压测试换成等价的确定性 barrier，不能恢复 timing-only flaky 检查；
- 不应回滚现有 queue 不可复活标志、worker CV interrupt 或 IO-loop-affine stream 修复，它们是进入重构前的安全底线。
