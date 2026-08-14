# 重构执行工作簿

> **权威关系：** [`migration-plan.md`](./migration-plan.md) 决定阶段顺序、进入条件和退出门禁；
> 本工作簿只记录该阶段内部的工作项、负责人产物和当前状态。两者冲突时以 migration-plan 为准，
> 并立即修正本工作簿。

## 如何使用

1. 先在 `migration-plan.md` 查看“当前执行阶段”；
2. 只执行本表中相同 `P阶段` 的工作项；
3. 工作项开始时改为 `DOING`，产物写入 `work-products/Pxx-*.md`；
4. 阶段的全部退出门禁通过后，才能进入下一阶段,并进行本次git提交；
5. 不按工作项编号跨阶段施工。编号只用于跟踪，不表示另一套路线。

状态：`TODO`、`DOING`、`DONE`、`BLOCKED`。

## 当前下一步

当前是 **P6（阶段 6，Provider 与 Result 垂直切片）**；P6-W1、P6-W2 已 DONE，
**下一工作项为 P6-W3 Retool Provider 垂直切片**。

P5-W1 已收口：runtime 显式构造并注册 chayns/retool，Registry 发布前冻结；
Controller、GenerationService、chatSession 与 reaper 全部改走 `IProviderRegistry`；
`ApiFactory/ApiManager`、静态注册宏和 `void* createApi` 已删除，新增防双轨门禁。
Debug 构建与 338/338 测试、全部架构门禁通过。产物见
[`P05-provider-registry.md`](../work-products/P05-provider-registry.md)。

P5-W2 已收口：`ResponseIndex` 与 `SessionExecutionGate` 由 AppContext 持有并通过
`IResponseIndex/IExecutionGate` 注入；GenerationService、ContinuityResolver、Controller 和
session rebind 均不再定位它们的 singleton，application/transport 也不再调用
`chatSession::getInstance()`。Debug 338/338 与全部门禁通过，详见
[`P05-session-services.md`](../work-products/P05-session-services.md)。

P5-W3 已完成主要 Controller locator 清理：Account/Channel/Health/Metrics/RetoolWorkspace
Controller 均改走注入端口，Workspace 业务编排进入 application use case，并新增增量棘轮；
Account application facade 已落地，AccountManager 与 Workspace provision 的业务 locator 已清理；
managedAccount backend/service 与 Retool Provider 已改为 composition-root 注入且 locator 归零；
session persistence/account settings/channel capability/telemetry 路径也已改为注入端口并归零 locator；
`chatSession` 与 `chaynsThreadReaper` 已改为 AppContext-owned lifecycle service，并新增 deadline
超支/二次启动回归与 singleton fallback 门禁；BackgroundTaskQueue、AccountManager workers 与
所有 concrete DB manager 的生命周期现已收口。BackgroundTaskQueue 是 AppContext-owned 的
`IBackgroundExecutor`（删除函数静态 adapter/queue locator），并显式构造 SessionDbManager 与
chaynsThreadDbManager、向二者注入该 executor；chayns provider 接收同一 ledger，账号 worker
owner 也紧跟 `AccountManager::init()` 登记。ErrorStatsService、ErrorStatsDbManager、StatusDbManager
与 ErrorStatsConfig 现均无 singleton：AppContext 显式持有 store/worker，worker 构造强制注入 sink，
定时清理使用 weak capture 并在 shutdown 注销。AccountManager、ChannelManager 与所有管理
Controller-facing application facade 均为 context-owned；最新 Workspace 增量使 `RetoolWorkspaceManager` 与
`RetoolWorkspaceService` 也改为 constructor-injected、AppContext-owned 对象，删除 singleton/
setter 双轨并在 rollback/shutdown 先撤销非 owning workspace port。其 concrete
`RetoolWorkspaceDbManager` 也已改为 AppContext-owned：构造无副作用，AppWiring 在 facade
初始化前显式 `initialize()` 并发布同一 store；未初始化使用只返回诊断错误。随后
`AccountDbManager`、`AccountBackupDbManager`、`ChannelDbManager` 与 `ConfigDbManager` 也完成同一
迁移：五个 concrete DB store 全部由 AppContext 唯一持有、显式初始化，均无 lazy singleton。
最后一个 AI transport seam 现已收口：`AiApiController` 只绑定 `IAiApiUseCase`，保留 HTTP/JSON
校验、路径推断和 JSON/SSE sink 适配；`sessionManager/core/AiApiUseCase` 独占请求规范化、queue
admission、legacy `GenerationService`、provider catalog 和 Responses persistence/read/delete 编排。
该 facade 由 AppContext 持有，rollback/shutdown 先 `AiApiController::setUseCase(nullptr)`；已接纳任务
在入队时 snapshot 借用协作者。Responses JSON/SSE sink 成功关闭时暴露 persistence record，因此 Controller
不再触碰 response index。374/374 `ctest`、直接 runner 374 cases / 1934 assertions 及全部架构门禁通过，
P5-W3 已满足“Controller 只依赖 use case”的退出标准。详见
[`P05-controller-services-progress.md`](../work-products/P05-controller-services-progress.md)。

P6-W1 已收口：`platform::Result/Error/ErrorCode`、绝对 `Deadline` 与只读
`CancellationToken` 已落地；legacy generation 的两个 ErrorCode 仅保留 platform alias。新的
JSON-free `ProviderRequest/Response/Capabilities/CallContext`、`IChatProvider` 和 final-NVI
`ProviderBase` 已建立，生产构造 helper 用 `static_assert` 禁止绕过基类。新增
`check_provider_foundation.py`（含 `[[nodiscard]]` C++ 编译 probe 与变异自检），并将 domain 的唯一
基础出边收紧为 ADR-01/02 已批准的 `domain -> platform`。本项刻意未把 legacy
`APIinterface/session_st&` 伪迁移；chayns 真正 slice 是下一项。Debug 构建、385/385 `ctest`、
直接 runner 385 cases / 1995 assertions 与全部门禁通过，详见
[`P06-provider-foundation.md`](../work-products/P06-provider-foundation.md)。

P4-W2 已于 2026-08-10 收口：C1～C8 全部 DONE。启动 27 步整体迁入 `AppContext::build()`，
`StartupResult` 使失败可观测并支持逆序 teardown，`shutdown(deadline)` 具备绝对截止时间与幂等性，
持线程单例经 `addOwner` 显式登记（G7 停机相关部分闭环）。新增门禁 `check_app_context.py`（A1～A5），
CI 门禁总数 11 项 + 2 项 selftest step，R4 selftest 引入 `assert_mutated` 前置断言以区分「探针失效」与「门禁失效」。
实测：Debug 构建 rc=0 / 0 warning，301 用例全过，11 项门禁全 rc=0。

P4-W3 已完成 D2～D7：统一取消/完成原语、五个 owner 的绝对 deadline 透传、限时
`joinUntil`、五类部署级 SIGTERM harness 和 `check_shutdown_deadline.py` 门禁均已落地；
Debug/ASan/TSan 三构建收口通过。
遗留至后续工作项：G7 的广义部分（`getInstance` 静态依赖收敛）归入阶段 5 注入改造；
G5 停机 deadline 的五类 SIGTERM 集成测试、ASan/TSan 全量收口已完成。

## 按 migration-plan 排列的工作项

| ID | migration 阶段 | 工作项 | 必交产物 | 状态 |
|---|---|---|---|---|
| P0-W1 | 阶段 0 | 当前真值与 clean baseline | [`P00-current-baseline.md`](../work-products/P00-current-baseline.md) | DONE |
| P1-W1 | 阶段 1 | gcov/llvm-cov 运行时覆盖基线 | [`P01-runtime-coverage.md`](../work-products/P01-runtime-coverage.md) | DONE |
| P1-W2 | 阶段 1 | Chayns 脱敏 fixture 与假上游 | [`P01-chayns-fixtures.md`](../work-products/P01-chayns-fixtures.md) | DONE |
| P1-W3 | 阶段 1 | Retool workflow/agent characterization | [`P01-retool-characterization.md`](../work-products/P01-retool-characterization.md) | DONE |
| P1-W4 | 阶段 1 | Generation/Account 权威实现 characterization | [`P01-generation-account-characterization.md`](../work-products/P01-generation-account-characterization.md) | DONE |
| P1-W5 | 阶段 1 | SIGTERM、队列、断连 harness | [`P01-shutdown-characterization.md`](../work-products/P01-shutdown-characterization.md) | DONE |
| P2-W1 | 阶段 2 | Provider 数据 dry-run 和归档/恢复脚本 | [`P02-provider-data-retirement.md`](../work-products/P02-provider-data-retirement.md) | DONE |
| P2-W2 | 阶段 2 | nexos/OpenAiProvider tombstone 与代码退役 | [`P02-provider-code-retirement.md`](../work-products/P02-provider-code-retirement.md) | DONE |
| P3-W1 | 阶段 3 | production target 唯一 source owner | [`P03-production-targets.md`](../work-products/P03-production-targets.md) | DONE |
| P3-W2 | 阶段 3 | 单一 include 根和完整路径 | [`P03-include-root.md`](../work-products/P03-include-root.md) | DONE |
| P3-W3 | 阶段 3 | 正式 target、首批闭包与 legacy ceiling | [`P03-layered-targets.md`](../work-products/P03-layered-targets.md) | DONE |
| P3-W4 | 阶段 3 | domain 模型与 JSON codec 分离 | [`P03-domain-codecs.md`](../work-products/P03-domain-codecs.md) | DONE |
| P4-W1 | 阶段 4 | 有界 executor 和四态队列 | [`P04-bounded-executor.md`](../work-products/P04-bounded-executor.md) | DONE |
| P4-W2 | 阶段 4 | AppContext/Builder/runtime lifecycle | [`P04-app-context.md`](../work-products/P04-app-context.md) | DONE |
| P4-W3 | 阶段 4 | deadline/cancellation/shutdown | [`P04-shutdown-deadline.md`](../work-products/P04-shutdown-deadline.md) | DONE |
| P5-W1 | 阶段 5 | ProviderRegistry/Router 注入 | [`P05-provider-registry.md`](../work-products/P05-provider-registry.md) | DONE |
| P5-W2 | 阶段 5 | SessionStore/ResponseIndex/Gate 注入 | [`P05-session-services.md`](../work-products/P05-session-services.md) | DONE |
| P5-W3 | 阶段 5 | Account/Channel/Workspace/Metrics 注入 | Controller 只依赖 use case | DONE |
| P6-W1 | 阶段 6 | Result/Error/ProviderBase 基础 | [`P06-provider-foundation.md`](../work-products/P06-provider-foundation.md) | DONE |
| P6-W2 | 阶段 6 | Chayns Provider 垂直切片 | [`P06-chayns-provider-slice.md`](../work-products/P06-chayns-provider-slice.md) | DONE |
| P6-W3 | 阶段 6 | Retool Provider 垂直切片 | workflow/agent contract 通过 | TODO |
| P7-W1 | 阶段 7 | Generation pipeline 重写 | stage contract、旧实现删除、R1 归零 | TODO |
| P7-W2 | 阶段 7 | Account workflows 重写 | selector/state machine/workers/回滚测试 | TODO |
| P8-W1 | 阶段 8 | 过渡代码和 debt 清理 | clean baseline、完整发布验证 | TODO |

## 产物保存规则

- 工作项产物保存在 `doc/adr/work-products/Pxx-*.md`；
- 每份产物包含输入、设计、调用图、测试结果、遗留问题和回滚方式；
- 机器数字只从审计/覆盖工具生成；
- 代码和文档可以分提交，但门禁未通过不得将工作项标成 `DONE`。
