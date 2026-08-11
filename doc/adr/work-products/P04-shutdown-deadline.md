# P04-W3 · deadline / cancellation / shutdown

> 状态：DOING（D1 现状刻画完成）
> 最近更新：2026-08-10 —— D1：五处 owner 停机链与 14 处阻塞边界 inventory 定稿。
> 计划锚点：`migration-plan.md` 4.3 取消链路 / 4.4 停机顺序 / 阶段 4 退出门禁
> 退出门禁（本工作项）：正常 SIGTERM 全部 join；deadline 超时路径不析构活动线程所访问对象；空闲/阻塞 HTTP/轮询/积压/断连五种停机集成测试通过

---

## 1. 前置与承接

P4-W2 已把启动收敛为 `config → build() → run() → shutdown(deadline)`，
并让 5 个后台 owner 经 `addOwner` 显式登记、逆序停机。但 `shutdown(deadline)` 目前
**只记录超支、不施加取消**——`AppContext::stopOwnersInReverse()` 在 deadline 已过时
仅打一条 WARN，随后照样阻塞调用 `it->stop()`。deadline 因此是账面上的，不是生效的。

本工作项要做的就是把 deadline 从「记录」变成「传播」，并给每个阻塞边界一条可取消路径。

---

## 2. 现状刻画（D1）：owner 停机链

登记顺序即启动顺序，逆序即停机顺序（由 `AppContext` 保证，不靠注释维持）：

| 登记序 | owner | 停机动作 | 会撞上的阻塞边界 |
|-------|-------|---------|----------------|
| 1 | error stats service | `ErrorStatsService::getInstance().shutdown()` | B10 wait_for + B9 join |
| 2 | chayns thread reaper | `chaynsThreadReaper::getInstance().stop()` | B6 wait_for + B7 sleep_for + B5 join |
| 3 | session cleaner | `chatSession::getInstance()->stopClearExpiredSession()` | B11 wait_for + B12 join |
| 4 | background task queue | `BackgroundTaskQueue::instance().shutdown()` | B14 wait_for + B13 join |
| 5 | account workers | `AccountManager::getInstance().stopBackgroundThreads()` | B2 wait_for + B3 join |

**停机实际顺序 = account workers → background task queue → session cleaner → chayns thread reaper → error stats service。**

---

## 3. 现状刻画（D1）：阻塞边界 inventory

扫描范围：`src/` 全部 `.cpp`/`.h`，排除 `src/test/`；剥离注释与字符串字面量后按偏移量匹配，
再把偏移量映射回行号。共 **14 处**。

| 编号 | 类别 | 位置 | 归属 owner |
|------|------|------|-----------|
| B1 | `sleep_for` | `src/accountManager/AccountClock.cpp:13` | —（provider 退避） |
| B2 | `cv_wait_for` | `src/accountManager/accountManager.cpp:392` | account workers |
| B3 | `thread_join` | `src/accountManager/accountManager.cpp:421` | account workers |
| B4 | `sleep_for` | `src/apipoint/chaynsapi/ChaynsClock.cpp:15` | —（provider 退避） |
| B5 | `thread_join` | `src/apipoint/chaynsapi/chaynsThreadReaper.cpp:76` | chayns thread reaper |
| B6 | `cv_wait_for` | `src/apipoint/chaynsapi/chaynsThreadReaper.cpp:89` | chayns thread reaper |
| B7 | `sleep_for` | `src/apipoint/chaynsapi/chaynsThreadReaper.cpp:188` | chayns thread reaper |
| B8 | `sleep_for` | `src/apipoint/retoolapi/RetoolClock.cpp:15` | —（provider 退避） |
| B9 | `thread_join` | `src/metrics/ErrorStatsService.cpp:96` | error stats service |
| B10 | `cv_wait_for` | `src/metrics/ErrorStatsService.cpp:235` | error stats service |
| B11 | `cv_wait_for` | `src/sessionManager/core/Session.cpp:735` | session cleaner |
| B12 | `thread_join` | `src/sessionManager/core/Session.cpp:775` | session cleaner |
| B13 | `thread_join` | `src/utils/BackgroundTaskQueue.h:157` | background task queue |
| B14 | `cv_wait_for` | `src/utils/BackgroundTaskQueue.h:184` | background task queue |

### 3.1 已剔除的假阳性：`future_get` 整类作废

中间版扫描器曾报出 7 处 `.get()` 视作 future 阻塞。逐个查证后全部证伪：
`channelStore` / `accountDbManager` / `channelDbManager` / `store_` 均声明为
`shared_ptr<I*Store>`，`.get()` 是取裸指针；`accountManager.cpp:1314` 的
`userName.second.get()` 同为 `shared_ptr` 解引用。反向确认：production 代码中
`std::future` / `std::async` / `std::promise` / `packaged_task` **零命中**，仅出现在 `src/test/`。
故该类别整体作废，不进入阻塞边界清单。

### 3.2 三处 `*Clock.cpp` 不归任何 owner

B1 / B4 / B8（`AccountClock` / `chaynsClock` / `RetoolClock` 的 `sleep_for`）是
provider 重试退避，位于请求处理路径而非停机路径。它们属于 migration-plan 4.3
取消链路的范畴，需随 `ProviderCallContext`（阶段 5）一并改造，**不在本工作项**
的 owner 停机链内。此处记录是为了说明「为什么 14 处里只有 11 处进 owner 映射」。

---

## 4. 缺口清单（D2~D7 的靶子）

| 编号 | 缺口 | 证据 | 收口于 |
|------|------|------|--------|
| **H1** | `RuntimeOwner::stop` 签名为 `std::function<void()>`，deadline 无法进入 owner 内部 | `AppContext.h:34` | D3 |
| **H2** | 无统一取消原语：`CancellationToken` 仅存在于 `SessionExecutionGate.h`，是会话级取消，与停机取消无关；Reaper 自造 `stopRequested_` 原子布尔 | `SessionExecutionGate.h:30`、`chaynsThreadReaper.h:63` | D2 |
| **H3** | 5 处 `cv_wait_for` 用相对时长而非绝对 `wait_until(deadline)`，跨 owner 累加会突破宽限期 | B2 / B6 / B10 / B11 / B14 | D4 |
| **H4** | B7（Reaper 删除间隔 `sleep_for`）不可中断，停机时最坏多等一个 `deleteSpacingMs`；且它位于同步 HTTP DELETE 循环内 | `chaynsThreadReaper.cpp:188` | D4 |
| **H5** | 5 处 `thread_join` 无超时上限，任一 worker 卡死即整条停机链卡死 | B3 / B5 / B9 / B12 / B13 | D3 + D4 |
| **H6** | `stopOwnersInReverse` 在 deadline 已过时只 WARN 不施加取消，deadline 不生效 | `AppContext.cpp:84-86` | D3 |
| **H7** | 五类部署级 SIGTERM 场景缺三类：现有 harness 只覆盖空闲路径，未覆盖阻塞 HTTP / 轮询 / 积压 / 断连 | `P01-shutdown-characterization.md:323` | D5 |

---

## 5. 子步骤与验收

| 步 | 内容 | 验收 |
|----|------|------|
| D1 | 本文档：owner 停机链 + 14 处阻塞边界 inventory + 7 个缺口定名 | ✅ 已完成 |
| D2 | 统一取消原语进 runtime/platform 层；Gate 与 Reaper 的局部实现归并 | 单测覆盖取消传播与幂等取消 |
| D3 | `RuntimeOwner::stop` 带绝对 deadline，5 处 owner 逐个改造，逆序语义不变 | H1/H5/H6 消解；逆序 teardown 单测仍绿 |
| D4 | 阻塞边界改可取消 `wait_until`：B2/B6/B7/B10/B11/B14 | H3/H4 消解；停机延迟从周期级降到毫秒级 |
| D5 | 五类部署级 SIGTERM 集成测试：空闲 / HTTP 阻塞 / 轮询 / 积压 / 断连 | 五类全绿，标记顺序齐全 |
| D6 | 新增门禁 `check_shutdown_deadline.py` + selftest 变异探针 | 探针实测 rc≠0，且 `assert_mutated` 前置断言通过 |
| D7 | 三构建（Debug / ASan+UBSan / TSan）+ 全门禁 + 文档收口 | 全绿，workbook 标 DONE |

---

## 6. 与计划的偏离（实施中产生，需留痕）

### E-1 · inventory 扫描器返工两次（D1 期间）

第一版正则要求 `wait_for(` 后紧跟 `lock`，而 `Session.cpp:735` 的实参换行书写，
结果**漏掉了会话轮询这一整类**——若照此写文档，H3 会少列一条、`session cleaner`
会被误判为无 `wait_for`。第二版改用「当前行 + 下一行」窗口拼接，把漏报换成了误报：
`Session.cpp:734` 的 `unique_lock` 声明被标成 `cv_wait_for`，`:774` 的 `notify_all()`
被标成 `thread_join`，命中数从 7 虚涨到 48。第三版改为「剥离注释与字符串 →
全文偏移量匹配 → 偏移量映射回行号」，命中 21 处，再人工证伪 `future_get` 7 处，定稿 14 处。

> 教训：**inventory 的数字必须能逐条点名，否则总数没有意义。**
> 7 / 48 / 21 / 14 四个数字里有三个是错的，而它们看上去都像「工具生成的机器数字」。
> 凡是要写进文档当验收依据的清单，都必须打印完整明细并抽样定点核对，不能只看聚合计数。

---

## 7. D3 开工前的现状复核（2026-08-10 晚）

D1 的 inventory 定稿于 `1a01f61`。此后 `1f9382d`（D11，停机可中断）改动了
`AppContext` 与 reaper/accountManager，**D1 的若干结论已失效**。本节按「逐条点名」
原则复核，不覆盖 §3/§4 原文，以便对照。

### 7.1 行号漂移复核（程序重算，非手抄）

| 编号 | 类别 | D1 行号 | 当前行号 | 判定 |
|------|------|--------|---------|------|
| B1/B2/B3/B4/B8/B9/B10/B11/B12/B13/B14 | — | — | 未变 | 11 处无漂移 |
| B5 | `thread_join` | 76 | **112** | `stop()` 拆为 `stop()/stop(deadline)/stopInternal()` 后下移 |
| B6 | `cv_wait_for` | 89 | **79** | 现为 `interruptibleSleepFor` 内的等待 |
| B7 | `sleep_for` | 188 | **已消失** | 见 7.3 |

`chaynsClock.cpp` 的路径在 §3 表中误写为 `chaynsClock.cpp`（首字母小写），实际为大写 `C`。

### 7.2 H1 已消解 —— 由 D11 顺带完成，不由 D3 完成

`RuntimeOwner::stop` 自 `1f9382d` 起为
`std::function<void(std::chrono::steady_clock::time_point)>`；
`stopOwnersInReverse` 已 `it->stop(deadline)` 并逐 owner 记录实耗；
`AppWiring` 五个 `addOwner` 闭包全部接收 deadline；
`test_app_context.cpp` 已有三条断言：`PassesExactDeadlineToOwners`、
`AllOwnersShareOneDeadline`、`RollbackAlsoDeliversDeadline`。

> 教训：**缺口清单也会过期。** D11 是为「后台线程停机可中断」开的，
> 却顺带把 D3 的主改造做了。若 D3 照 §4 表开工，第一件事就是重复实现一遍
> 已存在的签名改造。开工前复核缺口是否仍然存在，与开工前复核行号同等必要。

### 7.3 H4 已消解 —— B7 不再是不可中断睡眠

B7（删除间隔 `sleep_for(deleteSpacingMs)`）已被 `interruptibleSleepFor()` 取代
（定义 `:71`，调用点 `:225`），与 `loop()` 的周期等待共用
`wakeMutex_ / wakeCv_ / stopRequested_`，因此不存在「周期能打断、限速打不断」的偏差。
逐行删除循环的循环头亦有 `stopRequested_` 检查（`:189`），停机时剩余台账行留待下次启动回收。

仍不可中断的只剩**一次已发出的上游 DELETE**（30s 硬上限）；`stopInternal` 在剩余预算
小于该上限时如实 WARN。这是 D5 的验证对象，不是 D3 的改造对象。

### 7.4 H3 应为 6 处而非 5 处 —— D1 漏登记一处

reaper 内 `cv_wait_for` 实为两处：

| 新编号 | 位置 | 语义 |
|-------|------|------|
| B6 | `chaynsThreadReaper.cpp:79` | `interruptibleSleepFor`：删除限速等待（原 B7 的替代） |
| **B15** | `chaynsThreadReaper.cpp:125` | `loop()` 的扫描周期等待（`scanIntervalSeconds`） |

D1 的 §3 表把 reaper 的 `cv_wait_for` 只记了一条，故边界总数应为 **15**。
两处的相对时长语义都未改为绝对 `wait_until(deadline)`，**H3 对二者均成立**。

> 这正是 §6 E-1 那条教训的第二次应验：`grep` 计数为 2、文档登记为 1，
> 而聚合总数「14」看上去仍然像个可信的机器数字。

### 7.5 D3 的实际剩余范围（据此收窄）

| 缺口 | 复核后状态 | 归属 |
|------|-----------|------|
| H1 | ✅ 已消解（`1f9382d`） | 不需 D3 动作，仅需本节留痕 |
| H4 | ✅ 已消解（`1f9382d`） | 同上 |
| H6 | ⚠️ 半消解：deadline 已传播；超支仍只 WARN，不施加取消 | D3 剩余项 |
| H5 | ❌ 未做：5 处 `thread_join` 无超时上限 | **D3 主体** |
| H3 | ❌ 未做，且靶子数 5 → 6 | D4 |

H5 的硬约束：`std::thread` 没有限时 join，`join()` 一旦进入就无法带超时退出。
故只有三条路：

- **A. 线程自报完成 + 限时等标志**：worker 退出前置 `done` 并 notify，停机侧按剩余
  预算 `wait_until(done)`；超预算则不 join。现网 `BackgroundTaskQueue::waitUntilIdle`
  已是该模式的局部实例，可抽为通用原语与 D2 的 `CancellationSource` 并列。
- **B. 超预算即 `detach()`**：join 不再挂死，但线程仍在访问对象，与本工作项退出门禁
  「deadline 超时路径不析构活动线程所访问对象」直接冲突。
- **C. 承认 join 无上限，靠 D4 把所有等待改可取消来间接封顶**：不新增机制，但
  「任一 worker 卡在不可取消处（如已发出的上游 DELETE）即整链卡死」仍然成立。

倾向 A：它能给出「超预算」这一**可观测事实**且不引入 use-after-free 风险；
B 需与退出门禁一并重新定义，不在 D3 单独决定。

---

## 8. D4 开工前的现状复核（2026-08-11 凌晨）

### 8.1 §7.2 的 H1 结论需要打折：签名传播 ≠ 语义传播

§7.2 写「`AppWiring` 五个 `addOwner` 闭包全部接收 deadline」，据此判定 H1 已消解。
该陈述本身为真，但**据此推出的结论过宽**。判据：对 `AppWiring.cpp` 中 `deadline`
与 `budget` 的每一处引用，机械区分它出现在 `LOG_` 行/注释行还是普通语句行。

| owner | 登记行 | deadline 的实际用途 | 被调用的停机方签名 | 判定 |
|---|---|---|---|---|
| error stats service | `:144` | 仅 `LOG_INFO` 打印剩余预算 | `shutdown()` 无参 | ❌ 不参与控制流 |
| chayns thread reaper | `:204` | `stop(deadline)` | `stop(time_point)` | ✅ 参与 |
| session cleaner | `:254` | 仅 `LOG_INFO` 打印剩余预算 | `stopClearExpiredSession()` 无参 | ❌ 不参与控制流 |
| background task queue | `:295` | `waitUntilIdle(budget)` 且判返回值 | `waitUntilIdle(ms)` + `shutdown()` 无参 | ⚠️ 半参与：限时观测有，join 仍无上限 |
| account workers | `:322` | 与 `kAccountUpstreamRequestCap` 比较后 WARN | `stopBackgroundThreads()` 无参 | ⚠️ 仅用于告警，不改控制流 |

即：**5 个 owner 中只有 1 个把 deadline 真正用于控制流**，另有 2 个半参与。
`AppContext` 侧的 deadline 传播（D11 完成）是真的，但传播链在**最后一跳**断掉了——
owner 闭包拿到了预算，却调用了不接受预算的停机方。

> 教训：**「形参已存在」不能作为「语义已生效」的证据。**
> §7.2 只核对了闭包签名与 `test_app_context.cpp` 的三条断言（那三条验证的是
> AppContext 是否把同一个 deadline 交给各 owner，不验证 owner 拿它做了什么），
> 就把 H1 记为 ✅。判据必须落到「deadline 是否出现在非日志语句中」这种可机械
> 复算的形式上，否则复核会退化成读注释——而注释恰恰是最乐观的一方。
> 这是 §6 E-1 教训的第三次应验，前两次是计数错、清单过期，这次是**判据太弱**。

### 8.2 靶子清单重算（排除误命中）

`grep` 对 `wait_for|sleep_for|wait_until` 的 11 处原始命中中，4 处不是靶子：

| 命中 | 排除理由 |
|---|---|
| `Session.cpp:723` | 注释行，非代码 |
| `platform/Cancellation.h:73` | D2 成品，已是 `wait_until` |
| `platform/ThreadJoin.h:84` | D3 成品自身的 `join()` |
| `AccountClock.cpp:13` / `chaynsClock.cpp:15` / `RetoolClock.cpp:15` | 时钟适配器实现体，语义由调用方决定，不在停机路径 |

停机路径上真正需要绝对化的相对时长等待，重算为 **5 处**（§7.4 记为 6 处，其中
`chaynsThreadReaper.cpp:79` 的 `interruptibleSleepFor` 属删除限速、`:125` 属扫描周期，
二者确为两处，成立）：

| 编号 | 位置 | 语义 |
|---|---|---|
| W1 | `accountManager.cpp:392` | `backgroundSleep` 周期等待（最长 5 小时） |
| W2 | `Session.cpp:735` | clearExpired 周期等待（默认 1 小时） |
| W3 | `chaynsThreadReaper.cpp:79` | 删除限速等待 |
| W4 | `chaynsThreadReaper.cpp:125` | 扫描周期等待（默认 15 分钟） |
| W5 | `ErrorStatsService.cpp:235` | flush 批量等待（`asyncFlushMs`） |

停机路径上的无上限 `join()`，重算为 **5 处**（与 D1 的 H5 计数一致，但点名不同：
`ThreadJoin.h:84` 是 D3 自身，需排除；`accountManager.cpp:421` 在 lambda 内被调用
**4 次**，按代码位置计 1 处、按运行时线程计 4 条）：

| 编号 | 位置 | 线程数 |
|---|---|---|
| J1 | `accountManager.cpp:421` | 4（令牌巡检/令牌更新/账号数量/账号类型） |
| J2 | `Session.cpp:775` | 1 |
| J3 | `chaynsThreadReaper.cpp:112` | 1 |
| J4 | `BackgroundTaskQueue.h:157` | N（`worker_threads` 配置值） |
| J5 | `ErrorStatsService.cpp:96` | 1 |

### 8.3 W1~W5 是否真需要改？—— 逐条判定，不一律绝对化

五处等待都已带停机谓词，停机时靠 `notify_all` 立即唤醒，**在唤醒不丢失的前提下
它们都不会吃满周期**。把它们改成 `wait_until(deadline)` 的收益不是「更快停机」，
而是「**唤醒一旦丢失，等待仍有上限**」——即消除对「notify 一定送达」的依赖。

代价是每处都要拿到 deadline，而 W1/W2/W5 位于 worker 线程体内，deadline 只有停机
侧才有。三条路：

- **A. 把 deadline 存为成员**：停机侧 `store`，worker 侧 `load` 后 `wait_until(min(周期, deadline))`。
  需处理「未停机时 deadline 无意义」的初值问题。
- **B. worker 侧不改，只在停机侧用 `joinUntil` 封顶**：唤醒丢失时 join 超预算返回
  false 并告警，线程留给下次或进程退出回收。改动面最小，且 D3 原语现成。
- **C. 两者都做**。

倾向 **B**：W1~W5 的周期等待即使不绝对化，只要停机侧 join 有上限，整链就已封顶；
而 A 会在五个类里各引入一份「停机 deadline 成员」，与 D2 `CancellationSource` 的
职责重叠。是否补做 A 留待 D5 依据 J1~J5 的实测超支情况决定。

据此 **D4 的改造对象收窄为 J1~J5 + 五个 owner 闭包的透传**，W1~W5 本步不动，
但需在 §8.3 留痕说明「不动」是判断结果而非遗漏。

### 8.4 D4 实施留痕：ErrorStatsService（J5）限时停机 + 变异检验

按 §8.3 选定的路线 **B**（worker 侧不改，停机侧用 `joinUntil` 封顶）实施 J5。

**实现要点**

- 新增 `shutdown(deadline)` 重载，原无参 `shutdown()` 语义不变（无限等待），
  避免既有调用点行为被静默改写。
- 超预算时**不 join、不 detach、不做尾部落库、不复位 `initialized_`**：
  线程仍在访问队列，停机侧再并发 flush 只会制造竞争；复位标志更会在下次
  `init()` 时放出第二个 worker。调用方可稍后用无参重载重新收割。
- `ThreadCompletion` 每次 `init()` 重建。它是一次性信号，复用会让第二轮的
  限时汇合**静默退化成无限阻塞**——这是 D3 原语的使用前提，不是可选项。
- 顺带修掉一个既有缺陷：`shutdown()` 从不复位 `initialized_`，导致停过一次后
  `init()` 只打日志不再拉线程，服务变成**空壳单例**。

**变异检验（本步最重要的产出）**

| 变异 | 注入内容 | 结果 |
|---|---|---|
| A | 删除 `shutdown` 中的 `initialized_ = false;` | **红** — 约束成立 |
| B | `shutdown(deadline)` 忽略预算，改用无条件 `join()` | 初版**全绿，杀不掉** |

变异 B 暴露出：初版三个用例（`JoinsWithinBudget` /
`ExpiredDeadlineStaysBounded` / 重复 init）全部走「worker 秒退」的正常路径，
无限等待与限时汇合在该路径下耗时相同，**绿灯是免费拿到的**。
这与 §8.1 批评过的「判据被正常路径免费满足」是同一个毛病，
也是 §6 E-1 教训的**第四次应验**——前三次是计数错、清单过期、判据太弱。

补救：新增 `test_error_stats_shutdown_budget_exceeded.cpp`，经可注入的
`IErrorStatsSink` 塞入一个在 `insertEvents` 里长睡 3s 的 sink，把 worker 拖到
预算之外，再做**双侧**断言 —— `shutdown(200ms)` 必须返回 `false`，**且必须在
1s 内返回**。时间上界这一条才是杀手：变异 B 会等满 3s，必红。复验确认变异 B
在新用例下变红（326 用例中 1 failed）。

**判据升级（供 D5 沿用）**

> 时限类改造的验收，不能只看「正常路径下测试通过」。必须构造一条
> **让被限时对象真的超时**的路径，并同时断言**返回值语义**与**时间上界**。
> 做不到这一点，就等于没有验收——绿灯只证明了「快」，没证明「有上限」。

**遗留**

- `JoinsWithinBudget` / `ExpiredDeadlineStaysBounded` 单独看不具杀伤力，
  其价值在于覆盖正常路径与幂等语义；deadline 的真实约束由新用例承担。
  该结论已写入 `test_error_stats_shutdown_deadline.cpp` 文件头的「变异检验矩阵」
  注释，防止后人误判。
- J1~J4 尚未改造，仍走无限 join。

---

## 9. D5 自审：两类「看起来对」的缺陷及其机械判据（2026-08-11）

### 9.1 缺陷 A：注释写了、语义相反 —— `ErrorStatsService::isRunning()`

实读 `shutdownWithin()` 得到的事实：`running_` 在**持 `eventMutex_` 置 false 之后**
才进入 `joinUntil()`。因此限时停机超预算返回 `false` 时，`isRunning()` 已经报
`false`，而那条 worker 线程仍在运行、仍 `joinable`、未被 `detach`。

原 `.h` 注释称该函数用于「判断确实有东西可停」——与实现相反。调用方若据此判断
「已停干净」而跳过二次收割，留下的正是 D5 要消除的残局。

处置（不新增状态，只暴露既有事实）：

| 项 | 变更 |
|---|---|
| `isRunning()` | 注释改写为其真实语义「是否尚未请求停机」，并显式声明**不可**用于判断线程存活 |
| `hasPendingWorker()` | 新增，返回 `workerThread_.joinable()`，即「仍有线程待收割」 |

不变式：超预算后 `isRunning()==false` 且 `hasPendingWorker()==true`；无参 `shutdown()`
收割后两者同时为 `false`，且 `hasPendingWorker()` 不再回真。

### 9.2 缺陷 B：空断言 —— 断言的量在被测动作之前就已是期望值

`test_error_stats_shutdown_budget_exceeded.cpp` 原结尾为
`service.shutdown(); CHECK(service.isRunning() == false);`。
但 `running_` 早在上一行的**限时** `shutdown(deadline)` 里就已置 `false`，
因此这条 `CHECK` 对「二次收割是否真的 join 了线程」零覆盖——删掉那句 `shutdown()`
它照样绿。已替换为 `hasPendingWorker()` 的前后双断言。

### 9.3 判据：空断言的检出方法（变异，而非肉眼）

> 对每条断言问一句：**把它前面那句被测调用删掉，它还绿吗？** 还绿即为空断言。

本轮实测两例，均为「先预测红点位置、再跑、再核对」：

| 变异 | 操作 | 预测 | 实测 |
|---|---|---|---|
| T1 | 删 ErrorStats 用例中的二次收割 `service.shutdown()` | `hasPendingWorker()==false` 那条转红 | ✅ `:97 FAILED`，7 断言 1 红 |
| T2 | 删 BTQ `StaysDrainingAndReapable` 中的二次收割 `q.shutdown()` | `workerCount()==0` 与 `enqueue==Stopped` 转红 | ✅ `:101 :102 FAILED`，7 断言 2 红 |

T2 一并证明 BTQ 侧三个新用例**不含**空断言（`acceptingTasks()`/`workerCount()`
均由被测调用真实改变），即缺陷 B 是 ErrorStats 局部问题，未扩散。

两次变异后均已还原并复跑全量：**329 用例 / 1734 断言全绿**
（1734 = 变更前 1732 + 新增 2 条 `hasPendingWorker` 断言，计数吻合）。

> 与 §8.1 教训同源、第四次应验：「形参已存在」不证明语义生效，「断言已存在」
> 同样不证明行为被覆盖。两者的可信证据都只有一种形式——**指定一个预期会红的
> 位置，破坏它，看它是否真的在那里红。**
