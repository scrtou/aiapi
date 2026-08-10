# P04-W2 · AppContext / Builder / runtime lifecycle

> 状态：DOING（C1 刻画基线已完成，C2 起为改动步骤）
> 计划锚点：`migration-plan.md` 4.1 AppContext 骨架 / 4.4 生命周期迁移
> 退出门禁（本工作项）：启动失败回滚、显式 ownership

---

## 1. 范围与前置

P4-W1 已把 `BackgroundTaskQueue` 改成四态有界 executor，并把 `start()` 的调用权收回
composition root。P4-W2 接手的是 **composition root 本身**：`src/main.cc` 目前 441 行，
既是配置加载器、又是 CORS 装配点、又是 12+ 个单例的注入器、还是 4 个后台 owner 的停机编排者。

目标形态（4.1 原文四条）：

1. AppContext 先拥有配置、store、executor 和生命周期对象；
2. main 只负责 配置 → build → run → shutdown；
3. build 失败返回 Result，不留下半启动线程；
4. shutdown 接受统一绝对 deadline 且幂等。

落点已经存在：`aiapi_runtime` target 自 P3-W3 起就已声明，但当前只有
`utils/ApplicationShutdown.cpp` 一个成员，`src/runtime/` 目录尚未建立。

---

## 2. 现状刻画（C1）：启动序列

下表按 `main.cc` 实际执行顺序列出，**失败语义**一栏是本工作项要消灭或显式化的对象。

| # | 行 | 动作 | 执行上下文 | 失败语义 | 可回滚 |
|---|-----|------|-----------|---------|--------|
| 1 | 130-147 | `std::set_terminate` + backtrace | main 线程 | `_Exit(1)` | — |
| 2 | 150 | `loadConfigFile("../config.json")` | main 线程 | drogon 内部处理 | — |
| 3 | 151 | `ensureFilterReflectionRegistration()` | main 线程 | 无返回值 | — |
| 4 | 153-156 | `validateConfigFile()` | main 线程 | **`return 1` 真实生效**（唯一一处） | 无需（尚无线程） |
| 5 | 159-182 | CORS PreRouting advice | main 线程 | 无 | — |
| 6 | 185-196 | CORS PostHandling advice | main 线程 | 无 | — |
| 7 | 199-200 | `HealthController::setStartTime` | main 线程 | 无 | — |
| 8 | 202-204 | `registerBeginningAdvice`（仅日志） | main 线程 | 无 | — |
| 9 | 206 | `getLoop()->queueInLoop(...)` 包裹后续全部初始化 | **event loop** | — | — |
| 10 | 208-223 | `background_task_threads` 解析 + clamp 2..64 | event loop | 非整数 → WARN 降级默认 | — |
| 11 | 224 | `BackgroundTaskQueue::start(n)` | event loop | 无返回值 | 否 |
| 12 | 226 | `enqueue("init", ...)` 投递初始化闭包 | event loop | 见 #26 | 否 |
| 13 | 229-241 | `chatSession::setTrackingMode` | **队列 worker** | 未配置 → 默认 Hash | — |
| 14 | 245-246 | `ChannelManager::setStore` + `init()` | 队列 worker | 无返回值 | 否 |
| 15 | 249 | `AccountManager::setStore` | 队列 worker | 漏注入不崩溃 | 否 |
| 16 | 252 | `HealthController::setDbProbe` | 队列 worker | 漏注入 → `/ready` 恒 not_ready | 否 |
| 17 | 256 | `AccountManager::setChannelStore` | 队列 worker | 漏注入 → 渠道列表恒空，静默失效 | 否 |
| 18 | 257-258 | `setRetoolProvisionClock` | 队列 worker | 漏注入 → 回退默认 | 否 |
| 19 | 259 | `AccountManager::init()`（**起 4 个后台线程**） | 队列 worker | 无返回值 | 否 |
| 20 | 260-261 | `RetoolWorkspaceManager::setStore` + `init()` | 队列 worker | 无返回值 | 否 |
| 21 | 262 | `ApiManager::init()` | 队列 worker | 无返回值 | 否 |
| 22 | 268-271 | `ErrorStatsService::setSink` + `init()`（建表 + flush 线程） | 队列 worker | 漏注入 → 绕开端口静默落默认单例 | 否 |
| 23 | 276-292 | `SessionDbManager::ensureTables()` | 队列 worker | **失败 → 静默降级纯内存**（设计意图） | — |
| 24 | 298-334 | `chaynsThreadDbManager::ensureTable()` → `chaynsThreadReaper::start()` | 队列 worker | **失败 → 台账静默关闭**（设计意图） | — |
| 25 | 338-400 | 会话持久化参数 + `startClearExpiredSession()`（线程）+ `ResponseIndex` `runEvery` | 队列 worker | 无返回值 | 否 |
| 26 | 402-408 | `initEnqueued != Accepted` → `LOG_FATAL` + `return 1` | event loop | **见 G1：该 `return 1` 不终止进程** | — |
| 27 | 411 | `drogon::app().run()` | main 线程 | 阻塞至 SIGTERM | — |

### 现状刻画：停机序列

`lifecycle::runApplicationShutdown` 目前是固定四字段结构体 + 固定顺序的同步调用：

| 序 | 动作 | 顺序理由（源码注释） |
|----|------|--------------------|
| 1 | `chaynsThreadReaper::stop()` | 与队列**无**生产者-消费者关系；置首只保证其他 owner 尚未 teardown |
| 2 | `AccountManager::stopBackgroundThreads()` | 必须早于队列关闭，否则其投递的任务被 fail-fast 拒收并刷屏 |
| 3 | `chatSession::stopClearExpiredSession()` | 需在 DB 设施拆除前干净退出 |
| 4 | `BackgroundTaskQueue::shutdown()` | 最后 drain |

---

## 3. 缺口清单（C2～C7 的靶子）

| ID | 缺口 | 证据 | 归属子步骤 |
|----|------|------|-----------|
| **G1** | 入队失败的 `return 1` **静默失效**：它位于 `queueInLoop` 的 lambda 内，该 lambda 被包装成 `std::function<void()>`，返回值被丢弃。`LOG_FATAL` 打完进程继续运行，恰好落入注释所担心的「未初始化 Store 的半启动进程」 | main.cc 206 / 402-408 | C2 + C5 |
| **G2** | 初始化跨越三个上下文（main 线程 → event loop → 队列 worker），`run()` 已开始收请求时初始化未必完成，无任何 happens-before 保证 | main.cc 206 / 226 / 411 | C3 + C4 |
| **G3** | 12 处 setter 与 `init()` 的先后是**隐式**约束，仅靠注释 + `check_startup_wiring.py` 守住 | main.cc 245-271 | C4 + C7 |
| **G4** | 全部 `init()` 无返回值，失败不可观测，因此**回滚无从谈起** | 表 #14/19/20/21/22 | C2 + C5 |
| **G5** | 停机无 deadline：Reaper 可能阻塞在同步 HTTP DELETE，独占整个 SIGTERM 宽限期（P1 harness 已记录） | main.cc 415-423 | C6 |
| **G6** | `runApplicationShutdown` 非幂等，二次调用会重复执行四个 stop | ApplicationShutdown.cpp 15-32 | C6 |
| **G7** | ownership 全隐式：26 类全局单例（`AccountManager` 56 处、`RetoolWorkspaceManager`/`ResponseIndex` 各 44 处、`BackgroundTaskQueue` 29 处……），析构顺序由静态存储期决定 | 全仓 getInstance 统计 | C3 + C7 |
| **G8** | 两处「建表失败 → 静默降级」是**有意**行为（会话持久化、chayns 台账），迁移时必须保留为显式 `Degraded` 结果，不得被 fail-fast 吞掉语义 | main.cc 276-334 | C2 + C5 |

---

## 4. 目标形态

```
src/runtime/
  AppContext.h/.cpp        // 拥有 config 快照、executor 句柄、owner 列表
  StartupResult.h          // Ok / Degraded(reason) / Failed(code, detail)
  ApplicationShutdown.*    // 升级为带绝对 deadline 的 owner 列表 + 幂等
```

`main.cc` 收敛为：

```
config → AppContext::build(config) → 失败即 return 1（真实生效）
       → ctx.run() → ctx.shutdown(now + grace)
```

---

## 5. 子步骤与验收

| 步 | 内容 | 验收 |
|----|------|------|
| C1 | 本文档：启动 27 步 + 停机 4 步基线，8 个缺口定名 | ✅ 已完成 |
| C2 | `StartupResult` / 原因码 + `toString` | 单测覆盖每个原因码 |
| C3 | `AppContext` 骨架进 `aiapi_runtime`，`src/runtime/` 建立 | target 编译通过，层级门禁 rc=0 |
| C4 | init 闭包整体迁入 `build()`，顺序逐行保持 | `check_startup_wiring.py` 仍 rc=0 |
| C5 | 失败逆序 teardown；G1/G8 显式化 | 「第 N 步失败 → 前 N-1 步已 stop」单测 |
| C6 | shutdown 绝对 deadline + 幂等 | 二次调用 no-op 单测；harness 无回归 |
| C7 | `tools/arch/check_app_context.py`（第 11 个门禁 step） | 变异探针 rc≠0 |
| C8 | 全量三构建 + 全门禁 + 文档收口 | 全部 PASS / rc=0 |
