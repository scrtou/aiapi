# D11 停机路径 · 待议项

本轮（D11）只做了停机响应性改造，不改任何业务参数。以下两个数值在排查中被识别为
可疑，但改动它们会影响业务行为，故单独立项，留待决策。

## D11-Q1 账号登录/注册的单次上游超时 300.0 秒

**位置**

- `src/accountManager/accountManager.cpp:1068`（`getchaynsToken` 内，登录）
- `src/accountManager/accountManager.cpp:1738`（`autoRegisterAccount` 内，注册）

**现状**：两处均为写死的字面量 `300.0`（5 分钟）。同一文件内其余上游请求用的是
`30.0`；chayns 侧有具名常量 `chayns::kUpstreamRequestTimeoutSeconds = 30.0`。

**为什么是问题**：可中断化之后，账号线程停机耗时的上界就等于「一次已发出、无法撤回
的上游请求」，也就是这个 300.0。它直接决定了停机最坏耗时的量级（5 分钟 vs 30 秒），
也是 `AppWiring.cpp` 中那条预算告警的阈值来源。

**待决**：

1. 降到 30.0，与同文件其余请求及 chayns 口径一致？代价是慢上游下登录/注册成功率下降。
2. 保留 300.0，但提为具名常量，让告警阈值与实参共用同一个符号，避免日后改一处漏一处。
3. 是否应与 `chayns::kUpstreamRequestTimeoutSeconds` 统一——两者是否真属同一条上游链路，
   需要业务侧确认。

## D11-Q2 `isServerReachable` 的 `maxRetries = 300`

**位置**：`src/accountManager/accountManager.h:120`（默认实参），唯一调用点在
`accountManager.cpp:1050`（`getchaynsToken` 内），全仓无第二个调用方。

**现状**：每轮最坏 3 个探测路径 × 30 秒 + 1 秒间隔 ≈ 91 秒，300 轮 ≈ **7.6 小时**。

**为什么是问题**：这条链完全位于 `waitUpdateAccountToken` 工作线程上
（`updatechaynsToken` → `getchaynsToken` → `isServerReachable`），也就是停机要 join 的
线程之一。D11 已让它响应停机标志，因此不再阻塞停机；但在**非停机**场景下，一个后台
巡检线程为等待上游而占用 7.6 小时，更像是笔误而非设计。

**待决**：是否降到 10 量级？这会改变冷启动时等待上游就绪的行为（上游晚于本进程启动时，
重试窗口从 7.6 小时缩到几分钟），需确认部署环境中上游的实际就绪时间。

## D11-Q3 `updateAccountType` 首行无条件 `return`

**现象**：`accountManager.cpp` 的 `updateAccountType()` 第一行是
`LOG_INFO << "[账户管理] 不需要更新账号类型"; return;`，其后的渠道过滤、
`getUserProAccess()` 调用、变更落库分支全部不可达。

**来源**：提交 `ef1cc79`（2026-08-06，提交信息仅为时间戳 `08061752`，无说明）。

**定性：调试期临时冻结，非有意废弃。** 依据：
1. 只加了两行短路，其后的完整实现（含唯一调用者 `getUserProAccess`）原样保留；
   若是废弃，通常会一并删除。
2. 同提交内其他新增 `return` 均带条件，只有这一处是无条件的 —— 不是成批停用。
3. 同提交的主题是「chayns 浏览器指纹伪装 + free/pro 双路由」
   （新增 `CHAYNS_FREE_ORIGIN=https://sidekick.ki`、
   `CHAYNS_PRO_ORIGIN=https://mein.sidekick.ki` 与 `applychaynsRouteHeaders`）。
   该改造把 `accountType` 从「选号依据」升级为「决定请求走哪个 Origin/Referer」。
   在此期间冻结类型、避免巡检把类型改掉导致路由抖动，是合理的调试手段。
4. 此后碰过这几行的提交仅 `0d8cb46` / `9993967` / `396965c`，随后 20+ 个提交
   全在 P04 架构线上，无人回头解冻 —— 属被遗忘，而非有意保留。

**当前后果**（三处）：
- `accountTypeThread_` 在**空转**：`checkAccountTypeThread()` 于
  `enableBackgroundThreads` 下真的启动（预热 1 分钟，此后每 3 小时一轮），
  遍历全部账号只为逐个打一行「不需要更新账号类型」。
  注意旁边的注释「已改为事件驱动，不再定时检查」只对被注释掉的
  `checkAccountCountThread()` 成立，对本线程是错的。
- 两条事件驱动路径同样静默失效：新账号自动注册后（行 1914）、
  token 更新成功后（行 1419）。
- `accountType` 只在账号首次写入时由 `hasProAccess` 定型，此后永不更新。
  账号 pro 到期降级后系统仍按 pro 处理；且行 2309
  `deletableType = (accountType == "free")` 的回收判定随之失准。

**候选处置**（未决）：
- **A 解冻**：删掉那两行恢复原行为，同时把 T2 断言改强为 transport 调用计数
  （届时变异 M3 亦可被杀死）。风险：恢复对 `cube.tobit.cloud/ai-proxy` 的
  pro 探测出网请求，需确认当初冻结不是因为该接口本身出了问题。
- **B 彻底停用**：删除 `accountTypeThread_` / `checkAccountTypeThread` /
  `updateAllAccountTypes` / `updateAccountType` / `getUserProAccess`，
  并明确「`accountType` 写入时定型」。代价：pro 降级不被感知。
- **C 加开关**：参照已有的 `account_background_threads_enabled` 增配置项，
  默认沿用当前冻结态。最保守，但把决策继续后移。

**与本轮的关系**：T2（`AccountTypeRefreshLoopStopsImmediately`）因该 `return`
只能用耗时弱断言，故变异 M3（只删循环头停机检查、保留 `backgroundSleep`）
未被覆盖。本项处置后须回来把 T2 改强。

## D11 本轮已做（供对照，非待议）

- `backgroundSleep` 形参 `seconds` → `milliseconds`，以容纳亚秒级节流。
- `isServerReachable`：循环条件加停机检查；重试间隔由 `clock_->sleepFor` 改为
  `backgroundSleep`。
- `updateAllAccountTypes`：逐账号循环加停机出口；500ms 节流改为 `backgroundSleep`。
- `AppWiring.cpp` account workers 闭包：预算小于 300 秒时告警（阈值来源见 D11-Q1）。
