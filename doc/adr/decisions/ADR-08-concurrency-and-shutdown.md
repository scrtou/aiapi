# ADR-08 并发模型与停机时序

| 项 | 值 |
|---|---|
| 状态 | 已接受，待实施 |
| 来源 | RFC-001 v2.5 §2（原行 1737~1750），P4 拆分外移 |
| 迁移落点 | 见 [`migration-plan.md`](../migration-plan.md) |
| 数字真值源 | [`architecture-baseline.md`](../architecture-baseline.md) |

---

## 决策与理由

**决策**：

1. **阻塞位置**：事件循环线程内**禁止**同步 `sendRequest`、`sleep_for`、同步 DB 调用。
   所有含上游 IO 的执行**一律 enqueue 到 BackgroundTaskQueue**。流式路径已符合，非流式路径须改造。
2. **Pipeline 语义明文化**：§4.3 的 `Result<void> run(GenCtx&)` 是**同步阻塞签名**，整条 Pipeline 运行在后台线程；
   事件循环只负责接收 `IoLoopResponseStream` 推来的分片。把现在的隐式约定写成明文。
3. **定时任务统一用 loop 定时器**：禁止 `detach + while(true) + 长 sleep`。
   项目内已有正确先例 —— `ErrorStatsService.cpp:40` 的 `getLoop()->runEvery(hours(1), ...)`。
4. **停机时序**定为：停止接收新请求 → 停定时器与 Reaper → 各清理线程 `stop()+join()` → `BackgroundTaskQueue::shutdown()` 排空 → 关 DB 连接池。
5. **`enqueue` 改 fail-fast**：`shutdown()` 之后调用 `enqueue` 必须返回失败，**不得重启线程池**。
6. **与 ADR-06 合并**：上述时序由组合根 `AppContext::shutdown()` 承载 —— 这与「消灭单例」是同一件事，不单独设阶段。

---

## 补充条款（P5 补齐，v2.6）

原 ADR-08 六条决策都是**方向性表述**，缺三样东西：违规基数是多少、fail-fast 影响哪些调用点、停机各阶段的边界条件。以下逐项补齐。

### 8.1 决策 1 的违规基数：17 处 `sleep_for`，分布在 5 个文件

| 文件 | 性质 | 处置 |
|---|---|---|
| `accountManager.cpp` | 后台线程轮询睡眠 | **保留**（N4 已改条件变量可中断睡眠，属正确用法） |
| `Session.cpp` | 过期清理线程睡眠 | **保留**（同上） |
| `chaynsThreadReaper.cpp` | `deleteSpacingMs` 限速 | **保留**（后台线程内刻意限速，非事件循环） |
| `chaynsapi.cpp` | 轮询上游消息 | **需判定**：若在事件循环线程内即违规 |
| `retoolapi.cpp` | 轮询工作流状态 | **需判定**：同上 |

> **关键修正**：决策 1 写的是「禁止 `sleep_for`」，但实测 17 处中**至少 3 个文件是后台线程内的合法睡眠**。
> 条文应改为「**禁止在事件循环线程内** `sleep_for`」而非全局禁止 —— 否则 N4 刚做完的条件变量睡眠会被误判为违规，
> 反而诱导把已经正确的代码改坏。**真正待改造的只有 `chaynsapi` / `retoolapi` 两处轮询**，且必须先确认其执行线程归属。

### 8.2 决策 3 的现状：正确先例只有 1 处

全代码库 `runEvery` / `runAfter` 仅 **1 处**：`ErrorStatsService.cpp:40` 的 `runEvery(hours(1), ...)`。

> 决策 3 称之为「已有正确先例」是准确的，但 **1 : 17 的比例说明这是孤例而非既有惯例**。
> 迁移时不要假设「照着改就行」—— 需先确认 Drogon loop 定时器在本项目的可用性（单 loop / 多 loop、回调线程归属）。

### 8.3 决策 5 的影响面：12 个 `enqueue` 调用点

| 位置 | 处数 | fail-fast 后的行为要求 |
|---|---:|---|
| `SessionDbManager.cpp` | 5 | 会话/响应持久化 —— **失败必须记 ERROR**，否则数据静默丢失 |
| `chaynsThreadDbManager.cpp` | 5 | 上游线程记录写入 —— 同上 |
| `nexosapi.cpp:838` | 1 | `nexos_budget_exceeded` 标记 —— 失败可容忍（下次请求会重新触发） |
| `RetoolWorkspaceService.cpp:166` | 1 | 视阶段 0.5 决定是否还存在 |

**实测：`enqueue` 已返回 `bool`**（`BackgroundTaskQueue.h:50`），且 `shutdownCalled_` 是不可逆标志（N2 已修复线程池复活缺陷）。

> **所以决策 5「改 fail-fast」的机制部分已经完成**，未完成的是**调用侧**：12 个调用点目前全部忽略返回值。
> 补齐的实质工作是给这 12 处加失败处理，其中 10 处（两个 DbManager）属「失败即数据丢失」，必须至少落一条 ERROR 日志。
> **把决策 5 继续写成「改 enqueue」会让人去改一个已经改好的地方，而漏掉真正的 12 处调用侧。**

### 8.4 停机阶段机（补齐决策 4 的边界条件）

决策 4 给了五步顺序，但没给每步的**完成判据**与**超时行为**：

| 阶段 | 动作 | 完成判据 | 超时（建议） | 超时后 |
|---|---|---|---|---|
| S0 | 停止接收新请求 | Drogon 停止 accept | 立即 | — |
| S0.5 | **遍历 Gate 槽位广播 `cancel()`** | 全部 token 已置位 | 立即 | — |
| S1 | Reaper `stop()` | `worker_` join 返回 | 10s | 记 WARN，继续下一阶段 |
| S2 | 各清理线程 `stop()+join()` | 全部已 join | 5s | 记 ERROR |
| S3 | `BackgroundTaskQueue::shutdown()` | 队列排空 + worker 全部 join | 10s | 记 ERROR |
| S4 | 关 DB 连接池 | — | 2s | — |

> **总预算 ≤ 27s，卡在容器 SIGTERM 宽限期（通常 30s）内**。这是 S1 必须最先发起的量化依据 ——
> `main.cc` 注释说 Reaper「一轮可能耗时数分钟」，若不设超时，S1 单步就会吃光整个宽限期。
> **`chaynsThreadReaper::stop()` 目前没有超时参数**，这是 ADR-08 落地时要补的第一个接口改动。

### 8.5 新增 S0.5：`SessionExecutionGate` 的取消广播（原决策 4 完全缺失）

`SessionExecutionGate` 持有 `std::unordered_map<std::string, SessionSlotPtr> slots_`，
仅靠 `cleanup(maxIdleSlots = 1000)` 按空闲回收，**没有任何停机路径**（类中无 `shutdown()` / `stop()`）。

> 停机时若仍有槽位 `executing = true`，其 `CancellationToken` 不会被触发，对应 Pipeline 不知道该退出，
> 只能等 S2/S3 的 join 硬扛到超时。因此在 S0 之后、S1 之前插入 **S0.5：遍历 `slots_` 对全部 token 调 `cancel()`**，
> 让在跑的 Pipeline 尽早看到取消信号 —— 这一步能显著压低 S2/S3 的实际耗时，是 27s 预算成立的前提。
> 需要给 `SessionExecutionGate` 新增一个 `cancelAll()`。
>
> **状态：未实现**（P7-2 实测，`cancelAll` 在 `src/` 下命中 0）。
> 挂载点 `SessionExecutionGate.h:98` 与可复用的 `CancellationToken`（同文件 :30）均已存在，
> `tryAcquire` 的 `CancelPrevious` 分支（:133-135）已在调用 `currentToken->cancel()`，
> `cancelAll` 仅是把该行由单槽扩为全槽遍历，约 10 行。
> 实现约束与代码草案见 [migration-plan · N6](../migration-plan.md)。

### 8.6 与其它 ADR 的关系

- 与 [ADR-06](./ADR-06-composition-root.md) §6.2 **强耦合**：8.4 阶段机由 `AppContext::shutdown()` 承载；
  §6.2 的四步析构次序是本表 S1~S3 的展开，两者冲突时以 `main.cc` 源码注释的实测理由为准。
- 与 [ADR-05](./ADR-05-result-type.md)：8.3 的 `enqueue` 失败应产出 `Error` 而非静默 `bool`。
- 与 [ADR-07](./ADR-07-provider-template-method.md)：8.5 的取消令牌由 `ProviderBase` 骨架在阶段间检查。

---

## 8.1 / 8.2 数字修正（P5-11，统计口径含 `.cc` 后复测）

附录 C 的口径规则 1（扩展名必须含 `.cc`）一落地，8.1 与 8.2 的数字立刻变了。**原值是漏计。**

### 8.1 修正：grep 命中 18 处，其中真实调用点 17 处

| 文件 | 命中 | 说明 |
|---|---:|---|
| `accountManager.cpp` | 8 | 重试退避与轮询，均在后台线程 |
| `chaynsapi.cpp` | 4 | 退避 `BASE_DELAY`，需判定线程归属 |
| `retoolapi.cpp` | 3 | 轮询工作流状态，需判定线程归属 |
| `chaynsThreadReaper.cpp` | 1 | `deleteSpacingMs` 限速，后台线程 |
| `Session.cpp:724` | 1 | **不是调用** —— 是一条解释「为何用 `wait_for(谓词)` 而非 `sleep_for`」的注释 |
| **`controllers/AccountController.cc:478`** | 1 | **新暴露**，`sleep_for(seconds(5))` 位于 **Drogon 控制器**中 |

> **结论修正**：
> 1. **真实 `sleep_for` 调用点是 17 处，不是 18** —— 第 18 处是 `Session.cpp` 的注释。
>    N4 在那里留了「为何不用 `sleep_for`」的说明，反被 grep 计成违规，这是原始基数统计中的**假阳性**。
> 2. `accountManager.cpp` 有 **8 处**，是单文件最大集中地，8.1 原表把它笼统写作「保留」过于乐观 ——
>    8 处中含重试退避（1520、2436）与守护循环（1771~2070），性质不同，不能一刀切。
> 3. **`AccountController.cc:478` 是本次最重要的发现**：控制器方法默认跑在**事件循环线程**上，
>    在那里 `sleep_for(5s)` 会**阻塞整个 IO 线程 5 秒**，期间该线程上所有连接停止响应。
>    这正是决策 1 想禁止的那类代码，而它此前**从未被统计到**（因为只扫了 `.cpp`）。
>    → **列为 ADR-08 落地时的第一优先改造点**，优先级高于 `chaynsapi` / `retoolapi` 的轮询。

### 8.2 修正：`runEvery` 是 2 处，不是孤例

| 位置 | 周期 | 说明 |
|---|---|---|
| `ErrorStatsService.cpp:40` | `hours(1)` | 原 8.2 已记录 |
| **`main.cc:361`** | `cleanupMinutes * 60` 秒 | **新暴露**：`ResponseIndex::cleanup` 定时器，受配置门控 |

> **结论修正**：8.2 原文说「正确先例只有 1 处 / 是孤例而非惯例」**判断错误**。
> 实际有 2 处，且 `main.cc:361` 这一处更有参考价值 —— 它示范了
> **周期由配置驱动（`response_index.cleanup_interval_minutes`）+ 参数按值捕获进 lambda** 的完整写法，
> 比 `ErrorStatsService` 的硬编码 `hours(1)` 更接近迁移目标形态。
> 迁移时**应以 `main.cc:361` 为模板**，而非 8.2 原先指定的 `ErrorStatsService.cpp:40`。

> **附带教训**：8.1 与 8.2 的原始数字都是在「只扫 `.cpp`」口径下得出的，两处都偏低，
> 且漏掉的恰好是 **`main.cc`（组合根）与 `AccountController.cc`（控制器）** —— 架构上最关键的两个位置。
> 这与附录 C 规则 1 的预判完全一致：**漏 `.cc` 会系统性漏掉入口与控制器**。

---

## 勘误：撤回 8.1 关于 `AccountController.cc:478` 的判定（P5-12）

**上一节 8.1 中「`AccountController.cc:478` 是本次最重要的发现 / 列为第一优先改造点」的结论作废。**

### 实测事实

`AccountController::accountAutoRegister`（`AccountController.cc:423`）的流程是：

1. 校验参数（count 钳制在 1~20），构造 `status = "started"` 的响应；
2. 第 468 行 `BackgroundTaskQueue::instance().enqueue("accountAutoRegister", [apiName, count](){ ... })`；
3. 第 484 行 `ctl::sendJson(callback, response)` —— **在 enqueue 之后立即返回**。

第 478 行的 `sleep_for(seconds(5))` 位于**第 468 行 lambda 的函数体内部**（470~480 的注册循环里），
注释写明用途是「注册间隔 5 秒，避免过快」。它运行在 **BackgroundTaskQueue 的 worker 线程**，**不在事件循环线程**。

### 结论

- 这是**合法的后台限速睡眠**，与 `chaynsThreadReaper` 的 `deleteSpacingMs` 同类，**不违规、不需改造**。
- 8.1 把它排在 `chaynsapi` / `retoolapi` 之前的优先级判断**错误**，就此撤回。
- 同文件另一处 `enqueue("accountRefresh", ...)` 是同样的「立即响应 + 后台执行」模式，
  说明该控制器**本来就是照决策 1 的意图写的**。

### 方法论教训（比结论本身更重要）

> **错误链条**：grep 命中行号 → 看到文件名是 `Controller.cc` → 推断「控制器方法跑在事件循环上」 → 判定违规。
> 中间**跳过了「读该行所在的词法作用域」这一步**。行号不携带线程归属信息，
> 而 `sleep_for` 是否违规**完全取决于它被哪个可调用体包裹** —— 这是 grep 原理上无法回答的问题。

给 8.1 补一条**硬性判定程序**，排查每处 `sleep_for` 都必须走完：

| 步 | 动作 | 不可跳过的理由 |
|---|---|---|
| 1 | grep 定位行号 | 仅得到候选集 |
| 2 | 上溯到最近的 lambda / 函数体边界 | 决定线程归属的唯一依据 |
| 3 | 判断该可调用体由谁执行（`enqueue` / `std::thread` / 控制器直调 / `runEvery` 回调） | **同一文件不同行可以有相反结论** |
| 4 | 仅当执行者是事件循环线程时才计违规 | — |

> **代价评估**：18 个命中里，靠文件名猜测已在 1 处（`AccountController.cc`）判错、在 1 处（`Session.cpp:724` 注释）出假阳性，
> **错误率 2/18 ≈ 11%**。`chaynsapi.cpp`(4) 与 `retoolapi.cpp`(3) 这 7 处**至今未做第 2~3 步**，
> 8.1 给它们标「需判定」是诚实的，**但在完成判定前不得升级为「违规」**。

### 对 8.1 结论表的净修正

| 原判定 | 修正后 |
|---|---|
| 真实调用点 17 处（排除 `Session.cpp:724` 注释） | **维持** |
| `AccountController.cc:478` 为第一优先违规 | **撤回** —— 后台 worker 内合法限速 |
| `chaynsapi` / `retoolapi` 共 7 处「需判定」 | **维持**，且是唯一待查项 |
| `accountManager.cpp` 8 处性质不一 | **维持**，仍需逐处走判定程序 |

> 目前 ADR-08 决策 1 **尚无一处已确证的违规**。17 是候选集大小，不是违规数 ——
> 在完成判定程序前，不得把 17 写成「17 处违规」。

---

## 8.1 判定程序执行结果：7 处待判定 `sleep_for` 全部结案（P5-16）

P5-12 定下的四步判定程序，已对 `chaynsapi.cpp`(4) + `retoolapi.cpp`(3) 全部执行完毕。

### 第 2 步：词法作用域归属（大括号深度追踪）

| 命中 | 所属函数 | 函数区间 |
|---|---|---|
| `chaynsapi.cpp` 514 / 865 / 956 / 1026 | `chaynsapi::postChatMessage` | 283–1148 |
| `retoolapi.cpp` 842 | `retoolapi::requestWorkflow` | 733–847 |
| `retoolapi.cpp` 995 / 1301 | `retoolapi::requestAgent` | 847–1306 |

> 归属与「所有顶层函数定义行号表」逐一自洽，无跨函数错配。
> 值得单独记一笔：`postChatMessage` **一个函数 865 行**，四处 `sleep_for` 全在里面 ——
> 它同时是 ADR-08 的候选集中点与 baseline 「超长函数」名单成员，两条线索指向同一函数。

### 第 3 步：执行者追溯（完整调用链）

三个函数的上游收敛到**同一条链**，无第二条路径：

```
AiApiController (chat_nonstream_generation / chat_stream_generation)
  └─ BackgroundTaskQueue::instance().enqueue(...)        ← 线程切换点
       └─ GenerationService::executeProvider (GenerationService.cpp:463)
            └─ api->generate(session)                    (GenerationService.cpp:474)
                 ├─ chaynsapi::generate:251  → postChatMessage(session)
                 └─ retoolapi::generate:1320/1322 → requestAgent / requestWorkflow
```

关键事实：

- `api->generate(session)` 在全仓库**只有 1 个调用点**（`GenerationService.cpp:474`），
  排除 `apipoint/` 与 `test/` 后无其他命中 —— **provider 入口单点收敛**。
- `postChatMessage` 除 `chaynsapi::generate:251` 外无其他调用者
  （`OpenAiProvider::postChatMessage` 是同名不同类的独立实现，不构成第二条路径）。
- `requestWorkflow` / `requestAgent` 仅被 `retoolapi::generate:1320/1322` 调用。
- 控制器侧两个入口（172 非流式、337 流式）**都先 `enqueue` 再返回**，`callback` 在 lambda 内异步触发。

### 第 4 步：结论 —— **7 处全部合规，0 处违规**

这 7 处一律运行在 **BackgroundTaskQueue worker 线程**，不在 Drogon 事件循环线程。
性质是 provider 内部的**重试退避**（`chaynsapi` 的 `BASE_DELAY`）与**远端任务轮询**
（`retoolapi` 等待 workflow / agent run 完成），属后台线程的正当阻塞，**不违反决策 1**。

### ADR-08 决策 1 的最终基数（本轮排查终态）

| 类别 | 数量 | 说明 |
|---|---:|---|
| grep 命中 | 18 | 含 `.cc` 后的完整候选集 |
| 假阳性（注释） | 1 | `Session.cpp:724` 是「为何不用 `sleep_for`」的说明 |
| 真实调用点 | **17** | — |
| 已判定合规 | **9** | `chaynsapi`(4) + `retoolapi`(3) + `AccountController.cc:478` + `chaynsThreadReaper.cpp:180` |
| **仍待判定** | **8** | **全部集中在 `accountManager.cpp`**（1520/1771/1793/2030/2044/2049/2070/2436） |
| **已确证违规** | **0** | — |

> **重要的负面结论**：走完 17 处中的 9 处，**违规数仍是 0**。
> 决策 1「禁止事件循环内阻塞」在本仓库目前**找不到需要修复的对象** ——
> 它的真实价值是**防止未来退化**，而非清理存量。
> ADR-08 若写成「有 17 处存量待清理」，会误导实施者去做一批不存在的改造。

> **剩余风险集中度**：8 处待判定项全在 `accountManager.cpp` 一个文件里，
> 而该文件对应的 `accountManager.h` 是 baseline 中体量最大的单例头之一。
> 若要继续收敛基数，**唯一需要读的文件就是它** —— 但其体量意味着这不是一次 grep 能解决的事，
> 应作为独立任务立项，不要塞进本轮。

---

## 决策 1 收尾：`accountManager.cpp` 8 处结案（P6-4）

最后 8 处待判定项已走完四步判定程序。**决策 1 的候选集至此全部结案。**

### 第 2 步：词法作用域归属

| 命中 | 宿主函数 |
|---|---|
| 1520 | `isServerReachable`（1493–1525） |
| 1771 / 1793 | `checkChannelAccountCount`（1710–1807） |
| 2030 / 2044 / 2049 / 2070 | `autoRegisterAccount`（1807–2166） |
| 2436 | `updateAllAccountTypes`（2410–2443） |

> `autoRegisterAccount` 一个函数 **359 行**、内含 4 处 `sleep_for` ——
> 与 `chaynsapi::postChatMessage`（865 行 / 4 处）构成同一模式：
> **阻塞睡眠密度与函数长度正相关**，超长函数是睡眠逻辑的天然藏身处。

### 第 3 步：执行者追溯 —— 两类入口，均在后台线程

**入口 A：三个自有巡检线程 + 一个工作线程**

| 线程成员 | 启动函数 | 循环体 | 周期原语 |
|---|---|---|---|
| `tokenCheckThread_` | `checkUpdateTokenthread:1476` | `checkToken` + `cleanExpiredAccounts` | `backgroundSleep(hours(5))` |
| `accountCountThread_` | `checkAccountCountThread:1681` | `checkChannelAccountCounts` | `backgroundSleep(minutes(10))` |
| `accountTypeThread_` | `checkAccountTypeThread:2449` | `updateAllAccountTypes` | 预热 `minutes(1)` + `backgroundSleep(hours(3))` |
| `tokenUpdateWorker_` | `waitUpdateAccountTokenThread:1672` | `waitUpdateAccountToken` | 条件变量等待 |

**入口 B：6 个外部调用点，经深度追踪逐一验证，全部位于 `enqueue` lambda 内**

| 调用点 | 目标 | 判定 |
|---|---|---|
| `ChannelController.cc:79` / `:135` | `checkChannelAccountCount(s)` | ✅ enqueue 内 |
| `AccountController.cc:322` | `checkChannelAccountCounts` | ✅ enqueue 内 |
| `AccountController.cc:416` | `updateAllAccountTypes` | ✅ enqueue 内 |
| `AccountController.cc:472` | `autoRegisterAccount` | ✅ enqueue 内 |
| `nexosapi.cpp:840` | `checkChannelAccountCounts` | ✅ enqueue 内（`nexos_budget_exceeded_checkCounts`） |

`isServerReachable` 无外部调用者，仅被 `getchaynsToken:1182` / `getNexosToken:1270` 调用，随 token 链走后台线程。

### 第 4 步：结论 —— **8 处全部合规，0 违规**

### ADR-08 决策 1 的终局基数

| 类别 | 数量 |
|---|---:|
| grep 命中 | 18 |
| 注释假阳性 | 1 |
| 真实调用点 | **17** |
| **已判定合规** | **17（100%）** |
| **已确证违规** | **0** |

> **决策 1 的存量违规数为 0，排查完结。**
> 该决策不产生任何改造任务，其全部价值在于**约束未来新增代码**。
> 任何把它写成「存量清理项」的路线图都是错的。

---

## 新发现（本轮副产物，属决策 3 而非决策 1）：`backgroundSleep` 已存在但未被复用

`AccountManager::backgroundSleep`（`accountManager.cpp:448`）是仓库里**已写好的正确原语**：

- 基于 `backgroundWakeCv_.wait_for` + 谓词 `backgroundStopRequested_`；
- 返回值语义被统一为「是否应继续下一轮」：睡满返回 `true`，被停机唤醒返回 `false`；
- 配合 `stopBackgroundThreads`（幂等 `exchange` + 双条件变量 `notify_all` + 四次 `join`）
  构成完整可中断停机机制，`~AccountManager` 还做了兜底调用。

**问题在覆盖率**：`backgroundSleep` 仅 4 个调用点（1482 / 1687 / 2452 / 2462），
而裸 `sleep_for` 有 8 处。这 8 处虽都在后台线程（不违反决策 1），但**不可中断**：

| 位置 | 睡眠量 | 停机最坏延迟 |
|---|---|---|
| `autoRegisterAccount` 2030/2044/2049 | 3s × 3 | 单轮最多 9s |
| `autoRegisterAccount:2070` | 3s | 每轮注册叠加 |
| `checkChannelAccountCount` 1771/1793 | 5s + 5s | 每渠道叠加 |
| `updateAllAccountTypes:2436` | 500ms | 每账号叠加 → **与账号数成正比** |
| `isServerReachable:1520` | 1s × maxRetries | 探测重试期 |

**这才是真实缺陷**，性质与决策 1 不同：不是「阻塞了错误的线程」，
而是「**阻塞期间无法响应停机信号**」—— 属**决策 3（优雅停机）**范畴。

> `stopBackgroundThreads` 要 `join` 这四个线程。若 `accountCountThread_` 正卡在
> `checkChannelAccountCount` 的 5s 睡眠，或 `accountTypeThread_` 正在
> `updateAllAccountTypes` 里逐账号 500ms 睡眠（N 账号 = N×500ms），
> **`join` 就得等它睡完**。账号越多停机越慢，且该延迟**不设上界**。

### 建议（新增决策条目，非决策 1 的修复）

把决策 3 从「后台线程必须可 join」收紧为：

> **后台线程内的任何等待都必须经由可中断原语**（`backgroundSleep` 或等价的
> `condition_variable::wait_for` + 停机谓词），**禁止裸 `std::this_thread::sleep_for`**；
> 循环内每轮迭代都应检查 `backgroundStopRequested_`。

改造对象即上表 8 处。**`backgroundSleep` 已存在，无需新建抽象** —— 属纯替换，
但需注意部分命中位于 `for` 循环内部，替换后要把返回值接进 `break` 逻辑。

---

## 决策 3（收紧版）：后台线程内的**任何等待**必须经可中断原语

### 原表述与收紧后的表述

| | 表述 |
|---|---|
| 原 | 后台线程必须可 `join` |
| **收紧后** | 后台线程必须可 `join`，**且线程内任何等待必须经可中断原语**（`backgroundSleep` / 带停机谓词的 `wait`） |

**收紧理由**：原表述只约束「线程能不能退出」，不约束「多久才退出」。
实测 8 处裸 `std::this_thread::sleep_for` 全部满足原表述却让 `join` 被迫等睡满 ——
约束存在，但不足以达成它自己的目的。

### 为什么这不是决策 1 的违规（定性边界）

决策 1 管的是「**前台请求线程**不得阻塞睡眠」。这 8 处经 P6-4 逐点追踪，全部位于后台线程可达路径：

- `checkTokenThread` / `checkAccountCountThread` / `checkAccountTypeThread` 三个巡检线程内；
- `nexosapi.cpp:840` 的 `checkChannelAccountCounts()` **在 `BackgroundTaskQueue::enqueue` 的 lambda 内**（:838），
  即由后台队列线程执行，**不在 `markAccountBudgetExceeded` 的调用者线程上**。

故**决策 1 违规数仍为 0**，本条属决策 3 的独立缺陷。

### 逐点替换对照表（8 处）

`backgroundSleep(d)` 返回值语义：**`true` = 睡满，应继续；`false` = 被停机唤醒，应退出**（:448-456）。

| # | 行 | 所属函数 | 循环形态 | 替换方式 | 风险 |
|---:|---:|---|---|---|---|
| 1 | 1520 | `isServerReachable` | `while` 重试循环 | `if (!backgroundSleep(1s)) return false;` | ⚠️ 见下方 **例外 A** |
| 2 | 1771 | `checkChannelAccountCount` | `for (i<needed)` | `if (!backgroundSleep(5s)) break;` | 低 |
| 3 | 1793 | `checkChannelAccountCount` | `for (i<needed)` | `if (!backgroundSleep(5s)) break;` | 低 |
| 4 | 2030 | `autoRegisterAccount` | `for` + **`continue`** | `if (!backgroundSleep(3s)) break; continue;` | ⚠️ 见 **例外 B** |
| 5 | 2044 | `autoRegisterAccount` | `for` + **`continue`** | 同上 | ⚠️ 例外 B |
| 6 | 2049 | `autoRegisterAccount` | `for` + **`continue`** | 同上 | ⚠️ 例外 B |
| 7 | 2070 | `autoRegisterAccount` | `for` 尾部 | `if (!backgroundSleep(3s)) break;` | 低 |
| 8 | 2436 | `updateAllAccountTypes` | `for (accountsToUpdate)` | `if (!backgroundSleep(500ms)) break;` | ⚠️ 见 **例外 C** |

### 三个不能无脑替换的例外

**例外 A —— `isServerReachable` 的返回值语义（L1520）**

它不是 `void`，被停机唤醒时**必须返回 `false`（不可达）而非继续重试**。
两个调用点 `getchaynsToken`(:1182) / `getNexosToken`(:1270) 都以 `if (!isServerReachable(...)) return;` 短路，
返回 `false` 会让登录流程正常放弃 —— 语义正确，无副作用。

> 但注意：`isServerReachable` 是**普通成员函数，不保证只在后台线程调用**。
> 当前两个调用点确在后台，但一旦未来被前台复用，`backgroundSleep` 会让前台请求陪着后台停机信号一起醒 ——
> 语义虽不错，却是意外耦合。**建议 A 单独处理**：改为接受一个 `const std::atomic<bool>& stopFlag` 参数，
> 或最简单地保持现状并加注释说明「仅限后台调用」。

**例外 B —— `autoRegisterAccount` 的 `continue`（L2030 / 2044 / 2049）**

三处都是 `sleep_for(3s); continue;` 的形态。直接替换成 `if (!backgroundSleep(3s)) break;`
**会丢掉后面的 `continue`** —— 虽然此处 `break`/`continue` 后紧跟循环末尾、行为恰好等价，
但依赖「恰好等价」是脆弱的。**要求显式写全**：

```cpp
if (!backgroundSleep(std::chrono::seconds(3))) break;
continue;
```

该循环共 300 次 × 3 秒 ≈ **15 分钟**（`kWorkflowPollAttempts = 300`, :2011），
是全文件**单次停机延迟最长的一处**，收益最大。

**例外 C —— `updateAllAccountTypes` 的线性放大（L2436）**

500 ms × 账号数，**与账号数成正比且无上界**。这是收紧决策 3 的最初动因。
替换后 `break` 会让本轮剩余账号不再刷新 —— **可接受**：下一轮巡检（3 小时周期）会重新覆盖，
且 `checkAccountTypeThread` 的循环头 `while (!backgroundStopRequested_)` 本就会立即退出。

### 停机延迟改善估算

| 场景 | 现状最坏 | 收紧后 |
|---|---:|---:|
| `autoRegisterAccount` 轮询中 | ~15 分钟 | < 1 秒 |
| `updateAllAccountTypes` N 个账号 | N × 0.5 秒（无上界） | < 1 秒 |
| `checkChannelAccountCount` 补号中 | needed × 5 秒 | < 1 秒 |

### 门禁

新增 CI 检查：`accountManager` 目录下出现 `std::this_thread::sleep_for` 即失败，
例外需在行尾标注 `// ADR-08-D3-EXEMPT: <理由>`。

### 工期

**0.5 天**（8 处机械替换 + 例外 A 的接口决策）。目标原语已存在，无新抽象、无新依赖。
登记为 migration-plan 附录 A 的 **N7**。

---

## 决策 4（收紧版）：`BackgroundTaskQueue` 必须有显式状态机

### 问题：注释说排空，代码是 fail-fast

`BackgroundTaskQueue.h` 两处注释写「支持优雅停机（drain + join）」（:17）与「等待队列排空后」（:23），
但 `enqueue` 实际实现是拒绝（:60-63）：停机后直接 `return false`。**文档与代码互相矛盾**，
而 N2 的修复方向（fail-fast）是对的 —— 该改的是注释和**状态模型**。

### 根因：用三个 bool 拼状态

当前状态由 `started_` / `stopping_` / `shutdownCalled_` 三个独立 bool 表达。
`shutdownCalled_` 是 N2 补上的一次性标志，其注释（:56-59）自陈了原因：
`shutdown()` 结尾会置 `started_=false`，于是迟到的 `enqueue` 会重新 spawn 8 条线程并复位 `stopping_`。

这是**没有显式状态机的典型症状**：用补丁标志堵一个具体 bug，而不是让非法状态转移**不可表达**。
三个 bool 有 8 种组合，其中只有 4 种合法。

### 决策：四态状态机 + 可区分的入队结果

```
NotStarted ──start()/首次 enqueue──> Running ──shutdown()──> Draining ──worker 排空完毕──> Stopped
     │                                                                                        ▲
     └──────────────────────── shutdown()（从未启动）────────────────────────────────────────┘
```

**状态唯一，用单个 `std::atomic<State>` 表达；不再有独立的 `started_` / `stopping_` / `shutdownCalled_`。**
`Stopped` 与 `Draining` 均**不可逆**回 `Running` —— 这正是 `shutdownCalled_` 想表达而没表达好的性质。

### `enqueue()` 的返回值：从 `bool` 改为可区分结果

```cpp
enum class EnqueueResult { Accepted, QueueFull, ShuttingDown, Stopped };
[[nodiscard]] EnqueueResult enqueue(std::string name, std::function<void()> task);
```

| 状态 | `enqueue()` 结果 | 说明 |
|---|---|---|
| `NotStarted` | `Accepted` | 懒启动，保持现有行为 |
| `Running` | `Accepted` / `QueueFull` | `QueueFull` 需先定义容量上限（当前**无上限**，见待定项） |
| `Draining` | `ShuttingDown` | 拒绝新任务，但已入队任务仍会执行 |
| `Stopped` | `Stopped` | 拒绝；调用方应视为永久失败，不重试 |

调用方据此区分「稍后重试」（`QueueFull`）与「别再试了」（`ShuttingDown` / `Stopped`），
当前的 `bool` 把这两类混为一谈。

### 六条必须钉死的语义

| 问题 | 决策 |
|---|---|
| `shutdown()` 是否幂等 | **是**。非 `Running` 状态调用直接返回，可被 `AppContext::shutdown()` 与析构兜底重复调用 |
| `Draining` 期间任务内部递归提交任务 | **拒绝**（返回 `ShuttingDown`）。允许递归提交会让排空无法终止 |
| 未执行任务的处置 | **排空执行完**，不丢弃。丢弃语义留给超时兜底 |
| 谁拥有超时策略 | **调用方（`main.cc` 停机序列）**，非队列自身。队列只提供 `shutdown(timeout)` 参数 |
| 超时后行为 | 记录未完成任务数并**放弃等待**（不 detach、不强杀）；进程随后退出 |
| worker 内异常 | **捕获并记录任务名 + `what()`，不终止 worker**。异常不得穿越 worker 边界 |

### 与 ADR-08 §8.4 停机时序的对接

`Draining` 对应时序表的 S2；`Stopped` 是 S3 `join` 完成后的终态。
`main.cc:410` 的 `BackgroundTaskQueue::shutdown()` 即 `Running → Draining` 的触发点。

### 待定项（不在本次决策内）

**队列容量上限**：`QueueFull` 需要一个上限值，而当前 `tasks_` **无上限**。
定容量需要先测稳态队列深度 —— 未测之前不写死数字，`QueueFull` 暂为预留返回值。

### 工期

**1 天**，登记为 migration-plan 附录 A 的 **N9**（状态机重写 + 返回值改型 + 12 个 `enqueue` 调用点适配）。
注：调用点数见 §8.3。本项**改变公开接口**，不属热修，建议随阶段 1。
