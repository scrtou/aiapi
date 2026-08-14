# tools/arch —— 架构门禁（C4 / R4）

## 为什么需要它

ADR-02 靠 `target_link_libraries` 强制分层，而 **CMake 不允许 static library 循环依赖**。
只要 `src/` 存在跨模块环，阶段 1 的 target 拆分就无法完成。

更重要的是：阶段 0.7 的三环表是**逐条人工定位**的 —— 边看得很准，
但**看不出九个模块合起来是一个环**。这就是本脚本存在的理由。

## 用法

```bash
# 报告模式（不判定）
python3 tools/arch/check_cycles.py

# 带 file:line 证据
python3 tools/arch/check_cycles.py --evidence

# 门禁模式：实测结果必须是基线的子集，否则退出码 1
python3 tools/arch/check_cycles.py --baseline tools/arch/cycles-baseline.json

# 修完环之后收紧基线（棘轮只能往紧了拧）
python3 tools/arch/check_cycles.py --write-baseline
```

## 退出码

| 码 | 脚本 | 含义 |
|---:|---|---|
| 0 | 两者 | 通过 |
| 1 | check_cycles | 存在超出基线的环或双向边 |
| 2 | check_cycles | **前提被破坏**：出现跨目录同名头文件，基名判据失效 |
| 3 | check_cycles | `--layer-rules`：分层边界被破坏（如 domain/ 出现 platform 之外的出边） |
| 4 | check_cycles | `--db-ratchet`：出现棘轮清单外的 dbManager 直连 |
| 4 | check_startup_wiring | 注入缺失，或注入晚于 `init()` |
| 1 | check_source_ownership / check_include_paths / check_target_layers | owner 重复、include 非规范路径、DAG 或 legacy 棘轮回升 |
| 4 | check_test_registration | 测试文件未在 CMake 注册 |
| 4 | check_enqueue_result | queue `enqueue()` / executor `submit()` 丢属性，或调用点绑定后不读结果 |
| 4 | check_app_context | 组装根形状被破坏：build() 结果被丢弃、addOwner 逃出步骤、shutdown 收相对时长、main.cc 直接注入 |
| 4 | check_provider_registry | P5-W1 Provider 静态工厂/管理器复活，或显式注册/冻结接线缺失 |
| 4 | check_session_services | P5-W2 ResponseIndex/ExecutionGate 单例复活，或 application/transport 重新定位 chatSession |
| 4 | check_lifecycle_services | P5-W3 queue/session/thread/metrics、五个 concrete DB store、Workspace/Account/Channel 或 AI facade 生命周期 singleton fallback 复活，或 AppContext ownership/构造接线缺失 |
| 4 | check_provider_foundation | P6-W1 Result/Error/Deadline/CancellationToken、ProviderCallContext、薄 ProviderBase NVI、生产继承约束或 Result `[[nodiscard]]` compile probe 被破坏 |
| 4 | check_chayns_provider_slice | P6-W2 Chayns 重回 `APIinterface/session_st`、singleton、legacy registry lane，或丢失 Result/cancellation/thread/model capability 接线 |

> 码 4 在多个脚本里都用到，但它们是**各自独立的程序**，不共享码空间；
> workflow 中每道门禁是独立 step，不存在混淆。表里分列是为了让读者一眼看清归属。

## 判据说明（重要）

判据是「`#include` 的头文件**基名** → 该头文件在 `src/` 下的顶层目录」。

> **不要改回用 include 路径前缀判断模块归属。** v2.3 用过路径前缀，测出 **0 个环** —— 那是错的：
> 仓库里有 200 处 include 只写文件名，路径前缀判据完全看不见它们。

基名判据成立的前提是**头文件名全库唯一**。脚本每次运行都会先自检该前提，
不成立就直接退 2 —— 这同时也是 ADR-03 机械改写的守门条件
（重名状态下机械改写会静默改错目标）。

## 两个基线文件

| 文件 | 含义 |
|---|---|
| `cycles-baseline.json` | **当前实测态**（9 节点 SCC / 6 条双向边）。CI 用它防回归 |
| `cycles-target.json` | **阶段 0.7 验收标准**：SCC 仅剩 `{apipoint, sessionManager}`。阶段 0.7 完成时应能通过 |

阶段 0.7 的每一项（C1~C7）做完后，都应重跑 `--write-baseline` 收紧 `cycles-baseline.json`，
最终它应与 `cycles-target.json` 一致。

## 残留的那一个环

`{apipoint, sessionManager}` 由 `sessionManager/core/Session.cpp:6 -> chaynsapi.h` 造成，
是**真 DIP 违规**（domain 直接依赖具体 Provider），必须靠 `IChatProvider` port + 组合根注入才能断，
**已明确转阶段 2 与「消灭单例」同批处理**，不在阶段 0.7 范围内。

## 门禁 4：dbManager 直接依赖棘轮（退出码 4）

```bash
python3 tools/arch/check_cycles.py --db-ratchet tools/arch/db-include-ratchet.json
```

业务层直接 `#include` dbManager 头文件的**文件级**白名单。新增即 FAIL。

**为什么需要第四道**：前三道都看不见这类退化。
`accountManager -> dbManager` 是单向边，不成环（门禁 1/2 无感）；
该模块也已在 `layer-rules.json` 的 `allow_out` 里（门禁 3 放行）。
于是「在已白名单模块里无限追加 include」成了倒置成果被悄悄侵蚀的通道。
粒度定在**文件**而非模块，就是为了堵这条路。

**两类判定**：
- `allowed_files`：冻结现状。出现清单外的文件即 FAIL。
- `must_stay_clean`：已完成依赖倒置的模块，必须保持零直连。
  这是显式表达意图，而不是依赖「它恰好不在清单里」。

**工作流**：
- 完成一个模块的倒置后，把它从 `allowed_files` 移除、加入 `must_stay_clean`。
- 解除直连后脚本会提示 `--write-db-ratchet` 收紧清单。减少不算违规，不会 FAIL。
- 确需放宽，显式改 JSON 并在提交信息里写明理由。

**判据坑（实测记录）**：
扫描复用与前三道相同的头文件基名索引，原因是路径判据会漏两类写法——
无路径的 `#include "ErrorStatsDbManager.h"`，以及 `.cc` 扩展名的文件。
手工 grep 曾因此漏报 4 个文件（10 vs 实际 14）。


## 门禁 5：启动接线（退出码 4）

```bash
python3 tools/arch/check_startup_wiring.py
```

校验**组装根**中每条已登记的接线：注入**存在**，且**早于** `init()`。
组装根候选由脚本内 `WIRING_SOURCES` 列出，当前为 `src/runtime/AppWiring.cpp`、`src/main.cc`，按序取第一个存在的文件。P4-W2 把接线从 `main.cc` 搬进 `AppWiring.cpp` 后，原先钉死 `src/main.cc` 的判据会在**新文件里一条规则都找不到**却仍返回 0（假绿），故改为多候选 + 同文件内比较行号。
规则表 `REQUIRED` 为六元组 `(类, 接线名, 注入正则, init 正则, 说明, 漏接后果)`，FAIL 时打印该条各自的后果；
另有 `REQUIRED_STATIC` 只校验存在性（无对应 `init()` 时序）。当前共 11 条：
`ChannelDbManager.context-owned construction`、`ChannelManager.setStore`、
`ConfigDbManager.context-owned construction`、`AccountDbManager.context-owned construction`、
`AccountBackupDbManager.context-owned construction`、`RetoolWorkspaceDbManager.context-owned construction`、
`RetoolWorkspaceManager.constructor store injection`、`AccountManager.setStore`、
`AccountManager.setChannelStore`、`AccountManager.setRetoolProvisionClock`、
`HealthController::setUseCase`（静态）。

`ErrorStatsService` 不再属于本表：P5-W3 已将它和两个 metrics store 迁为 AppContext-owned
对象，sink 在构造函数中强制提供，相关生命周期与无 singleton fallback 的判据由
`check_lifecycle_services.py` 负责。

**为什么需要这一道**：这道守的是单元测试在结构上覆盖不到的地方。
倒置后实现由组合根注入，注入语句漏写时**编译通过、全部单元测试通过**
（R4 试点 B 期间真实发生过，183 项测试全绿），运行期却静默退化为 Null 实现。
单元测试自行注入 Fake，不经过启动路径，因此再多的单元测试也堵不住这个洞。

**为什么要校验顺序而非仅校验存在**：`init()` 内部会建表并写入默认数据。
注入若晚于 `init()`，这些写操作全部落到 Null 实现上，症状与漏注入相同。

**漏接后果并非只有一种**（这也是规则表升为四元组的由来）：`AccountManager.setStore`
漏注入会空指针崩溃（步骤 86 实测）；`setChannelStore` 漏注入不崩溃，但渠道列表恒空、
自动补注册静默失效。早期版本对所有规则统一打印「退化为 Null 实现」，退出码正确但
描述失真，会把排障者引向错误方向，现已按规则分列。

**已知局限**：判据是正则文本匹配，只认当前的 `std::make_shared` / 局部注入写法，且注入与 `init()` 必须落在**同一个**组装根文件里（跨文件顺序无法比较行号）。
若注入被包进辅助函数（如 `wireDependencies()`），检查会**误报 FAIL**。
当前各 Manager 写法一致，尚可接受；写法一旦分化就必须改判据。

**自检探针**：workflow 的 gate selftest 中，probe C 删除注入断言 rc=4，
probe D 把注入移到 `init()` 之后断言 rc=4。缺 probe D 的话，
「只搜字符串不看顺序」的退化改法会全绿通过。

> **探针必须随组装根一起迁移。** P4-W2 迁移后 probe C/D 一度仍在改 `src/main.cc`——
> 它们删的是一个已不在那儿的东西，门禁自然 rc=0，`expect_rc 4` 随即失败。
> 这次是 selftest 自己把这处失效抓了出来（也正是它存在的理由）；
> 两个探针已改钉 `src/runtime/AppWiring.cpp`，probe C 另加了「没删掉任何行就直接 FAIL」
> 的空改判定，避免将来再次静默变成一个什么都没破坏的探针。

## 门禁 10：后台 executor 结果处理（check_enqueue_result，退出码 4）

```bash
python3 tools/arch/check_enqueue_result.py
```

校验 `BackgroundTaskQueue::enqueue()` 与 `IBackgroundExecutor::submit()` 的返回值在全部生产
调用点均被真正读取，且两个声明处仍带 `[[nodiscard]]`。

**为什么需要这一道**：`[[nodiscard]]` 只拦得住「整体丢弃返回值」一种写法。
写成 `const auto ignored = ...submit(...);` 之后再不读 `ignored`，
编译器最多给一条 `-Wunused-variable`，而本项目未开 `-Werror` —— 绿色照旧。
P5-W3 把 application/transport/DB 写穿从具体 queue 收敛到 `IBackgroundExecutor`，但队列满 /
停机中的提交失败仍不能重新变成「已受理」的假象。

**两条判据**（都必须过）：
- queue 与 port 声明处均保留 `[[nodiscard]]`；防止有人为消警告删属性。
- 每个调用点：要么就地判断（`if (...submit(...) != Accepted)`），
  要么绑定局部变量且该变量在**同一函数体内**被读取。

**为什么作用域限定在函数体内**：多个调用点都会使用 `r` / `counted` 等局部名。
若判据放宽到全文搜索变量名，任意别处的同名变量都会让本处误判为「已读取」。

**已知局限**：文本作用域近似，不做数据流分析。把结果存进成员变量、跨函数再读的写法
会**误报 FAIL**。当前调用点均为局部变量就地判断；写法一旦分化必须改判据，
不要靠加白名单绕过 —— 白名单会让这道门禁退化成一张过期清单。

**自检探针**（本轮实测，两条都断言 rc=4）：
- probe A：删除 queue `[[nodiscard]]` → FAIL，指向头文件；
- probe B：把一个 `IBackgroundExecutor::submit()` 调用点改成绑定后从不读取 → FAIL，
  指向 `ChannelController.cc`。
  缺 probe B 的话，「只检查头文件属性」的退化改法会全绿通过。

## 门禁 11：组装根形状（check_app_context，退出码 4）

```bash
python3 tools/arch/check_app_context.py
```

P4-W2 把启动流程从 `main.cc` 的双层 lambda 搬进 `AppContext` + `AppWiring` 之后，
有五条性质完全靠「代码现在恰好长这样」维持，编译器与单测都抓不住。这道门禁把它们钉死。

**五条判据**（任一不满足即 rc=4）：

| 判据 | 内容 | 退化后果 |
|---|---|---|
| A1 | `main.cc` 中 `build()` 的返回值必须被接住并据以决定退出 | 写成裸调用或 `(void)` 强转即复发 G1：建表失败被吞，进程带着未建表的 Store 继续 `run()` |
| A2 | 每个 `addOwner` 必须位于某个步骤的 lambda 内部 | 逃到 `registerApplicationSteps` 顶层后，`build()` 中途失败会去 `stop` 一个从未 `start` 的 owner |
| A3 | `shutdown()` 只接受绝对 deadline（`steady_clock::time_point`） | 改回相对时长不会有编译错误，但跨 owner 逐段累加会突破 SIGTERM 宽限期，即 G5 复发 |
| A4 | `main.cc` 不得再出现直接的 Store 注入 | 接线出现第二处真相后，门禁 5 反而会因「在候选文件里搜到了」而通过 —— 两道门禁一起变瞎 |
| A5 | 登记册中的每个持线程 runtime service 都有 owner stop 接线 | 某个 worker 会重新依赖隐式析构，绕开统一 deadline 与停机日志 |

**为什么前十道都看不见**：这类退化不成环、不跨层、不改 CMake、不加测试文件，
注入语句也确实还在（只是搬错了地方或丢了返回值），十道全绿。
A4 尤其要紧：它守的是**门禁 5 的前提**，即「组装根只有一处」。

**自检探针**（本轮实测，两条都断言 rc=4）：
- probe J1：把接住 `build()` 返回值的那行改成裸调用 → FAIL；
- probe J2：在 `registerApplicationSteps` 顶层插一条 `addOwner` → FAIL。
  缺 J2 的话，「只看 main.cc 调用形状」的判据会对 owner 逃逸完全无感。

**已知局限**：与门禁 5 同源 —— 文本 + 结构近似，不做语义分析。
`AppWiring.cpp` 若改名或 `registerApplicationSteps` 若重命名，
必须同步更新脚本与两个探针；脚本在找不到锚点时会显式报错而非静默通过。
### `check_controller_services.py`

P5-W3 完成棘轮：`AccountController`、`ChannelController`、`HealthController`、
`MetricsController`、`RetoolWorkspaceController` 与 `AiApiController` 必须各自只发布一个
controller-facing use case，并禁止所有 Controller 恢复 `getInstance()/instance()`。runtime 必须保留
Account/Channel/Metrics/Workspace 与 `AiApiUseCase` 接线，且 Metrics Controller 不得直接 include DB
manager。`AiApiController` 只能绑定 `IAiApiUseCase`，不得重新持有 provider/session/index/gate/channel/
executor 或 `GenerationService`；这些 legacy collaborator 只允许在 concrete facade 内协调。

### `check_account_services.py`

P5-W3 Account 增量门禁：AccountManager 与 `AccountAdminUseCase` 不得定位任何 singleton，
并要求 runtime 保留 config/workspace/use-case 三组接线。

### `check_managed_account_services.py`

P5-W3 managedAccount 增量门禁：Classic/Retool backend、`ManagedAccountService` 和
Retool Provider 不得定位 singleton；同时要求 composition root 构造两个 backend、
非 singleton service，并把其生命周期发布到 `AppContext` 后再注入 Provider。

### `check_session_application_services.py`

P5-W3 session application 增量门禁：ResponseIndex/Session/RequestAdapters/GenerationService/
Bridge telemetry 不得定位或 include 具体 Manager/DB/metrics service；同时要求 runtime 保留
session persistence、account settings、channel catalog 与 telemetry sink 的完整接线。

### `check_lifecycle_services.py`

P5-W3 lifecycle 增量门禁：`AccountDbManager`、`AccountBackupDbManager`、`ChannelDbManager`、
`ConfigDbManager`、`RetoolWorkspaceDbManager`、`RetoolWorkspaceManager`、
`RetoolWorkspaceService`、`chatSession`（SessionJanitor）、`chaynsThreadReaper`、
`BackgroundTaskQueue`、SessionDbManager、chayns thread ledger 与 metrics 的
ErrorStatsService/ErrorStatsDbManager/StatusDbManager 必须是 AppContext 持有的普通对象；
ErrorStatsConfig 也不得恢复 process singleton。门禁同时检查五个 concrete store 的显式
初始化与 context publish、manager/provisioner 的 constructor injection 与 rollback unpublish、
ThreadReaper/Provider 的 ledger 注入、metrics 的强制 sink 构造注入、queue deadline-aware owner，
以及 concrete DB 写穿只依赖注入 `IBackgroundExecutor`。它补住的是 controller/use-case 门禁看不见的
“第二个静态 owner 或 workspace/queue/DB/metrics locator 绕过停机链”退化；`AiApiController` 唯一的
静态非 owning `IAiApiUseCase` binding 必须由 owner 在 rollback/shutdown 时
`setUseCase(nullptr)` 撤销，避免 context 析构后悬垂。已接纳的 generation task 必须由 facade 使用
入队时捕获的 collaborator snapshot，不能在 queue worker 中重读已被撤销的 Controller binding。

### `check_provider_foundation.py`

P6-W1 foundation gate:

```bash
python3 tools/arch/check_provider_foundation.py
```

它冻结 `platform::Result/Error/ErrorCode`、绝对 `Deadline`、只读
`CancellationToken`、JSON/Drogon-free `ProviderRequest/Response/CallContext` 与
`IChatProvider` 的最小契约。它还要求 `ProviderBase::generate()` 是 `final` NVI，保留
cancellation/deadline 前置检查、异常转换、结果合法性和一次失败上报；任何生产源码直接实现
`IChatProvider` 都会失败，生产构造 helper 必须保留 `ProviderBase` 的 `static_assert`。

`[[nodiscard]]` 不只靠文本搜属性：脚本使用一个不依赖 Drogon 的 C++17 `-fsyntax-only`
probe，以 `-Werror=unused-result` 分别编译“正确消费 Result”和“丢弃 Result”的小程序；前者必须
通过、后者必须失败。CI 另有变异自检，临时移除泛型 `Result` 的属性后要求 gate 返回 4。

P6-W1 只建立契约，尚未让 legacy `APIinterface` 或 chayns/retool 生产实现接入该 port；这些真实
vertical slice 在 P6-W2/P6-W3 完成。为使 domain 使用 Result/Deadline/取消契约，layer rules 现在
允许 ADR-01/02 明确的目标态 `domain -> platform`，但仍禁止 domain 指向任何具体 IO/codec 模块。

### `check_chayns_provider_slice.py`

P6-W2 Chayns vertical-slice gate:

```bash
python3 tools/arch/check_chayns_provider_slice.py
python3 tools/arch/check_chayns_provider_slice.py --selftest
```

它冻结 Chayns 的 `ProviderBase` NVI、`IProviderModelCatalog` 与
`IProviderThreadContext` 能力；生产目录不得再出现 `session_st`、`APIinterface`、
`Session.h`、`session.response` 或项目 singleton 查询。运行时必须由 production factory
构造并仅通过 `registerChatProvider("chaynsapi", ...)` 发布，GenerationService 必须优先走
`IChatProvider`，session/reaper 必须走窄 thread capability，Retool 的宽 port fallback 则仍被
明确保留到 P6-W3。它还检查 CancelPrevious 的 lease identity，避免被取消的旧请求释放新请求。

`--selftest` 不写工作树：在内存中破坏 Chayns 的 `ProviderBase` 继承，再要求同一判据拒绝它。
