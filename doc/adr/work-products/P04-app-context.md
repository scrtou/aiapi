# P04-W2 · AppContext / Builder / runtime lifecycle

> 状态：DONE（C1~C8 全部完成）
> 最近更新：2026-08-10 —— C8 收口：三构建（Debug / ASan+UBSan / TSan）与 13 项架构门禁全绿，证据见第 7 节。
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
| **G7** | ownership 全隐式：26 类全局单例（`AccountManager` 56 处、`RetoolWorkspaceManager`/`ResponseIndex` 各 44 处、`BackgroundTaskQueue` 29 处……），析构顺序由静态存储期决定 | 全仓 getInstance 统计 | C3 + C7 | —— 持线程部分已于 §8 收口（A5 + probe J3）
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
| C2 | `StartupResult` / 原因码 + `toString` | ✅ 单测覆盖每个原因码 |
| C3 | `AppContext` 骨架进 `aiapi_runtime`，`src/runtime/` 建立 | ✅ 三构建通过，`check_target_layers.py` rc=0 |
| C4 | init 闭包整体迁入 `build()`，顺序逐行保持 | ✅ rc=0，但**判据本身被迫同步改造**，见下方偏离 D-1 |
| C5 | 失败逆序 teardown；G1/G8 显式化 | ✅ 逆序 teardown 单测通过；G8 两处保留为显式 `Degraded` |
| C6 | shutdown 绝对 deadline + 幂等 | ✅ 二次调用 no-op 单测通过；见偏离 D-2 |
| C7 | `tools/arch/check_app_context.py`（第 11 个门禁 step） | ✅ A1~A4 四条判据；probe J1/J2 实测 rc=4 |
| C8 | 全量三构建 + 全门禁 + 文档收口 | ✅ 三构建各 301 用例全过；13 项门禁 rc=0；TSan data race=0（第 7 节） |

---

## 6. 与计划的偏离（实施中产生，需留痕）

### D-1 · `check_startup_wiring.py` 判据被迫改造（影响 C4 验收的成立条件）

C4 的验收写的是「`check_startup_wiring.py` 仍 rc=0」。实施后它确实 rc=0，
**但这个 0 一度不成立**：该脚本原本硬编码 `src/main.cc`，接线迁到 `AppWiring.cpp` 之后，
它在 `main.cc` 里一条规则都找不到，走的是「无事可查」的分支，照样返回 0。
也就是说，如果照字面签收 C4，签下的会是一个已经瞎掉的门禁。

处置：脚本改为在 `WIRING_SOURCES = ['src/runtime/AppWiring.cpp', 'src/main.cc']`
中按序取第一个存在的文件，并要求注入与 `init()` 落在同一文件内以便比较行号；
候选清单里一个都不存在时直接判失败，而不是静默通过。

> 教训：**「门禁 rc=0」不等于「门禁在看着它该看的东西」。**
> 凡是把被检查对象搬家的步骤，都必须同时回答「门禁现在指向哪儿」。

### D-2 · `ApplicationShutdown.*` 未按目标形态保留为独立文件

第 4 节把 `ApplicationShutdown.*` 列为 `src/runtime/` 下的独立单元。实际实施中，
owner 列表与幂等标志已经是 `AppContext` 的成员，独立文件只剩一个转发函数，
G6（非幂等）在 `AppContext::shutdown()` 里就地解决更直接。故删除该文件，
职责并入 `AppContext`；新增的 `AppWiring.cpp` 承接原 `main.cc` 的接线段。
目标形态一节保留原样不改，以便对照——这里记录差异本身。

### D-3 · gate selftest 的 probe C/D 随本步一起失效（本轮修复）

probe C/D 是门禁 5 的自检探针，靠「破坏 `src/main.cc` 再断言 rc=4」工作。
本步把接线搬走后，它们删除的是一个已不在 `main.cc` 的字符串——文件没变，
门禁返回 0，`expect_rc 4` 失败。这是 selftest 第一次抓到「探针自身过期」，
也印证了它的价值：**没有 selftest，这两个探针会静默变成永远通过的摆设。**

处置：两个探针改钉 `src/runtime/AppWiring.cpp`；probe C 另加空改判定
（grep 后文件若与原文件逐字节相同则直接 FAIL 并提示同步更新），
避免将来再次退化成「什么都没破坏」的探针。

---

## 7. C8 收口证据（2026-08-10）

### 7.1 三构建回归

| 构建 | 配置 | 单测 | 结果 |
|------|------|------|------|
| Debug | `build/`，无 sanitizer | 301 cases / 1645 assertions | ✅ build rc=0，test rc=0 |
| ASan+UBSan | `build-asan/`，`-fsanitize=address,undefined`；`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` | 301 cases / 1645 assertions | ✅ 无 AddressSanitizer / runtime error |
| TSan | `tools/run-tsan.sh`（含停机专项 x5、信号夹具 x5） | 301 cases / 1645 assertions | ✅ 告警合计 2，**data race=0**；夹具 5/5 标记齐全 |

TSan 的 2 条告警均为 Drogon 信号路径固有的 signal-unsafe（`queueInLoop` 中的 `operator new`），
非数据竞争，脚本按既定判据只对 data race 计失败——见 `tools/run-tsan.sh` 判据说明。

### 7.2 架构门禁（按 CI `arch-cycles.yml` 顺序全量本地复跑）

13 项全部 rc=0：`architecture_audit --selftest` / `--baseline`、`check_cycles`
（`--evidence` / `--baseline target` / `--layer-rules` / `--db-ratchet`）、
`check_startup_wiring`、`check_test_registration`、`check_source_ownership`、
`check_include_paths`、`check_target_layers`、`check_enqueue_result`、`check_app_context`。

### 7.3 退出门禁对照

| 本工作项退出门禁 | 兑现处 |
|------------------|--------|
| 启动失败回滚 | C5 逆序 teardown 单测；G1/G8 显式化为 `Failed` / `Degraded` |
| 显式 ownership | C3/C7 `AppContext` owner 列表 + `check_app_context.py` A1~A4 判据 |

### D-4 · TSan 只认 `tools/run-tsan.sh` 这一个入口（收口时再次踩到）

本轮收口一度直接裸跑 `build-tsan/src/test/aiapi_test`，得到非零退出码并险些记成用例失败。
实际原因是缺少 `TSAN_OPTIONS`：没有 `exitcode=0`，TSan 会用自己的退出码覆盖测试结果；
没有 `suppressions=tsan.supp`，trantor 既有告警也会一并冒出来。

> 教训：**判据脚本本身就是判据的一部分**。TSan 结论只在 `tools/run-tsan.sh` 下成立，
> 裸跑二进制得到的红/绿都不作数；文档与 README 引用 TSan 结果时一律指向该脚本。

---

## 8. G7 收口证据（2026-08-10）

C8 收口时 G7 仍是「ownership 全隐式」的开放缺口。本轮把最后一处**持线程单例**
交还给 `AppContext` 编排，并给守住它的判据补上负向探针。

### 8.1 缺陷本体

`ErrorStatsService::init()` 会拉起 `workerThread_`，但接线处没有任何 `addOwner`，
停机全靠 `~ErrorStatsService()` 里的兜底 `shutdown()`。这条兜底路径有两个问题：

1. **时机在编排之外**：静态析构发生在 `main` 返回之后，早已越过 `shutdown(deadline)`，
   停机日志对它只字不提——超时也无从计量。
2. **依赖可能已先死**：同为静态存储期的 `ErrorStatsDbManager` 与 drogon DB 客户端
   销毁顺序不确定，兜底里的 flush 可能落在已析构对象上。

这正是 G7 描述的隐式 ownership 在**持线程**场景下的具体复发形态。

### 8.2 修复与门禁

| 项 | 内容 |
|----|------|
| 代码 | `AppWiring.cpp` 中登记 `ctx.addOwner("error stats service", ...)`，停机按 owner 逆序收尾 |
| 判据 | `check_app_context.py` 新增 A5：登记册内每个持线程单例都必须在接线处出现 `addOwner` |
| 探针 | CI selftest 新增 probe J3：摘掉该 `addOwner` 后门禁必须转红（`expect_rc 4`） |
| 回归 | Debug 构建 rc=0；301 cases / 1645 assertions 全绿；13 项架构门禁全部 rc=0 |

A5 采用**双判据**：既查具体的停机调用是否存在，也查 `addOwner` 出现次数是否
少于登记册规模。前者定位到具体服务，后者兜住「新增持线程单例却忘了登记」。
probe J3 触发时两条同时报出，符合预期。

### 8.3 探针为何删「注册」而不删「函数体」

删函数体同样能让 `check_app_context.py` 退 4，但那是计数判据在起作用，
证明不了「漏登记」这条判据活着。探针必须精确命中被看守的那一条，
否则它退化成 D-3 里记过的那种「什么都没破坏也能绿」的假探针。

### D-5 · 探针正则在多行 `addOwner` 上失配（本轮踩到）

probe J3 初版用 `[^;]*;\n` 截取待删行。实际 `addOwner` 的 lambda 参数换行书写，
否定字符类在 lambda 体内的**第一个分号**处就停住，之后又要求紧跟行尾——必然失配。
而失配时探针脚本 `raise SystemExit(1)`、门禁照常 rc=0，`expect_rc 4` 报失败，
表面看像门禁坏了。已改为惰性跨行匹配 `.*?\);\n`。

> 教训：**探针的正则也要按判据对待**。凡是匹配 C++ 调用点的模式，
> 都必须假定实参会跨行；写完立刻用「从 yml 原文抽出 heredoc 实跑一遍」验证，
> 而不是只在本地凭记忆重敲一份等价脚本——两者不是同一段字节。

### 8.4 G7 状态

持线程单例的显式 ownership 已闭环并由 A5 + probe J3 双向看守。
`getInstance` 总量仍高（G7 的广义部分），但那属于后续工作项的静态依赖收敛范畴，
与停机安全解耦：**不持线程的单例即使靠静态析构收尾，也不会越过 `shutdown(deadline)`**。
