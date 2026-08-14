# P5-W3 Controller 服务注入（完成记录）

## 输入

- P5-W1/W2 已完成 Provider 与 Session 服务注入；
- `ChannelController`、`HealthController`、`MetricsController`、
  `RetoolWorkspaceController` 仍直接定位 Manager/DB/后台队列；
- P5-W3 退出标准要求每个业务 Controller 只依赖一个 controller-facing use case；最后待收口的
  边界是直接协调 provider/session/gate/executor 的 `AiApiController`。

## 设计与调用图

```text
AppWiring (composition root)
  ├─ owns BackgroundTaskQueue : IBackgroundExecutor
  │    ├─ SessionDbManager(executor) -> ISessionPersistence
  │    └─ chaynsThreadDbManager(executor) -> chaynsapi / ThreadReaper
  ├─ owns ChannelDbManager + ConfigDbManager
  ├─ owns AccountDbManager + AccountBackupDbManager
  ├─ owns ErrorStatsDbManager + StatusDbManager
  │    └─ ErrorStatsService(IErrorStatsSink) -> MetricsController / telemetry bridges
  ├─ owns RetoolWorkspaceDbManager : IRetoolWorkspaceStore
  │    ├─ RetoolWorkspaceManager with the same injected store
  │    └─ IRetoolWorkspaceProvisioner
  │         └─ RetoolWorkspaceService with the same injected store
  ├─ owns ChannelManager + AccountManager + all management use cases
  ├─ IAccountAdminUseCase                  -> AccountController
  ├─ IChannelAdminUseCase                  -> ChannelController
  ├─ IHealthUseCase                        -> HealthController
  ├─ IMetricsUseCase                       -> MetricsController
  ├─ IRetoolWorkspaceAdminUseCase          -> RetoolWorkspaceController
  ├─ owns AiApiUseCase(provider/session/response/gate/channel/executor)
  │    └─ IAiApiUseCase                     -> AiApiController
  ├─ owns chatSession -> SessionJanitor owner
  └─ owns chaynsThreadReaper(ledger) -> ThreadReaper owner

RetoolWorkspaceUseCase (application)
  -> IRetoolWorkspaceStore / IKeyValueConfigStore / IChannelCatalog

ManagedAccountService
  -> ClassicProviderAccountBackend
       -> IAccountCatalog / IAccountAdminCommands
  -> RetoolWorkspaceBackend
       -> IRetoolWorkspaceUseCase
  -> retoolapi (IManagedAccountContextResolver 注入)
```

- Metrics 查询模型下沉到 `domain/model/MetricsData.h`，Controller 不再 include DB manager；
- Channel 的账号数核算通过 `IBackgroundExecutor` 提交，闭包只捕获注入服务；
- Workspace 合并、启停、验证和池状态聚合进入 application use case，Controller 只做 HTTP/JSON；
- Provider provision 的 JSON 边界暂由字符串端口承载，保证 domain 无 JsonCpp；
- managedAccount 不再内嵌具体 backend 或暴露 singleton，两个 backend 由 composition root
  构造，service 生命周期由 AppContext 持有并在 Provider `init()` 前注入；
- Retool Provider 的 workspace 选择、usage 计数、上下文解析和渠道查询全部走注入端口；
- 未注入 Channel 服务时内置渠道更新明确返回 503，不再空指针解引用。
- `chatSession` 删除测试兼容 singleton；所有 fixture 改为局部 store，AppContext 是 production
  唯一 owner。SessionJanitor 的 blocking persistence 测试现在能验证 deadline 超支时返回
  `false`、保留 worker 供显式收割，以及二次 start 重建 completion。
- `chaynsThreadReaper` 改为构造注入 `chaynsThreadDbManager`，由 AppContext 持有；停机 owner
  捕获同一实例，不再从静态入口重新定位 reaper。
- `BackgroundTaskQueue` 实现 `IBackgroundExecutor` 并由 AppContext 唯一持有；原来的
  `BackgroundExecutorAdapter` 函数静态对象与 `BackgroundTaskQueue::instance()` 已删除。
  queue owner 捕获相同的 shared object，因此注入给 HTTP、application 和 DB 写穿的 executor
  与实际 drain/join 的 executor 必为同一个。
- SessionDbManager 与 chaynsThreadDbManager 改为 runtime 显式构造并接收 executor；异步写穿
  只调用注入端口，缺注入时记录丢失写入而不恢复 global fallback。两者以 `shared_from_this`
  保持队列任务期间的 store 生命周期。chayns provider 也接收 context-owned ledger。
- ErrorStatsDbManager、StatusDbManager 与 ErrorStatsService 同样由 AppContext 持有；Service
  构造时必须接收 `IErrorStatsSink`，不再 `setSink()` 或回退具体 DB singleton。ErrorStatsConfig
  在 AppWiring 从 `custom_config.error_stats` 解析为值对象；清理 timer 用 weak capture，并由
  shutdown 注销。停机 owner 先解除 Controller/bridge 的非 owning 指针，再停同一 worker。
- AccountManager、ChannelManager、AccountAdminUseCase 与 RetoolWorkspaceUseCase 均为
  AppContext-owned；AccountManager 实现窄 `IAccountSelector`，chayns 只接收该端口。
  AccountManager 的 `init()` 拉起 worker 后立即在同一 startup step 登记 owner，provider 初始化
  失败时 rollback 不再遗漏已启动的账号线程。
- RetoolWorkspaceManager 与 RetoolWorkspaceService 现只允许 construction injection：Manager 为每个
  实例保留诊断性的 Null store，Service 对空 store 立即拒绝构造；二者由 AppContext 持有。Controller
  与 AccountManager 的 raw workspace port 会在 rollback/shutdown 前先撤销，避免它们跨越 context
  析构边界。RetoolWorkspaceDbManager 也不再是 lazy singleton：其构造无副作用，AppWiring 在
  Workspace facade 任何初始化之前显式 `initialize()`，再作为 concrete store 由 AppContext 持有。
  未初始化的 store 会返回诊断错误，不会空解引用或恢复全局 fallback。
- `AccountDbManager`、`AccountBackupDbManager`、`ChannelDbManager` 与 `ConfigDbManager` 同样删除
  lazy singleton：构造无副作用，AppWiring 显式 `initialize()` 并在其借用方 `init()` 前将同一 concrete
  store 发布到 AppContext；因此五个 concrete DB store 都只有 context-owned 生命周期。
- `AiApiController` 的唯一静态非 owning binding 是 `IAiApiUseCase*`。AppContext 持有 concrete
  `AiApiUseCase`；rollback/shutdown 时先 `AiApiController::setUseCase(nullptr)`，再析构 facade 借用的
  provider/session/gate/executor。facade 在 queue admission 时 snapshot 这些协作者，已接纳 task 不会
  在 worker 中重读已撤销的 Controller binding。遗留 `GenerationService` 仍在 facade 内部，留待 P7
  按 pipeline contract 拆解，而不再泄漏到 Controller。

## 测试与门禁

- 新增 Account/Channel/Health/Metrics/Workspace 管理 Controller 的 use-case 注入回归测试；
- 新增 RetoolWorkspaceUseCase 的部分更新保密字段、校验、池状态和验证测试；
- 新增 `tools/arch/check_controller_services.py`，冻结已收口的管理 Controller：
  禁止 `getInstance()/instance()` 复活并要求 runtime 接线存在；
- 新增 managedAccount service 分派测试，Retool provider fixture 改为每用例显式组装 fake
  store/use-case/backend/service，不再修改全局 Manager；
- 新增 `tools/arch/check_managed_account_services.py`，冻结 managedAccount 与 Retool Provider
  的 locator inventory 并要求 runtime 完整接线；
- 新增 blocking `ISessionPersistence` fake 回归，覆盖 deadline 超支、显式 reap 与第二次
  启动的 completion 重建；
- 新增 `check_lifecycle_services.py` 并接入 CI，冻结 SessionJanitor/ThreadReaper 的显式 ownership；
- 扩展 lifecycle gate：冻结 context-owned BackgroundTaskQueue、SessionDbManager 与 thread ledger，
  并扩展为冻结 ErrorStatsService、ErrorStatsDbManager、StatusDbManager 与 ErrorStatsConfig；
  禁止恢复 queue/DB/metrics singleton 或 DB manager 对具体 queue 的 include；
- 扩展 `check_enqueue_result.py`：同时检查 `BackgroundTaskQueue::enqueue()` 和
  `IBackgroundExecutor::submit()` 的 `[[nodiscard]]` 及生产调用结果使用；
- 新增 AppContext queue-as-executor identity 回归，以及 SessionDbManager recording executor
  回归，验证异步写穿只走被注入的端口；
- 新增 AppContext metrics worker/store ownership 回归；ErrorStats sink、deadline 与超预算测试
  改为局部 service，证明不再依赖跨用例 singleton 状态；
- RetoolWorkspaceManager store-port 测试改为每用例局部 fixture；新增 provisioner 必须接收 store
  和 AppContext 作为 Workspace manager/provisioner 唯一 owner 的回归；
- 新增 AppContext concrete Workspace store ownership、以及 store 必须经 runtime 显式初始化的回归；
- 扩展 AppContext ownership 回归，覆盖 Health/Metrics/Channel/Workspace-admin 四个新增 facade；
- lifecycle/startup-wiring gate 扩展为禁止恢复三项 Workspace singleton/setter，并检查 concrete
  store 的 context ownership、construction/initialize 顺序、runtime construction 与 rollback
  unpublish 接线；
- lifecycle/startup-wiring gate 继续扩展到 Account/AccountBackup/Channel/Config 四个 concrete
  DB store，冻结显式初始化、AppContext ownership 和“先 publish、后借用方 init”的顺序；
- 新增 `IAiApiUseCase` 的 Controller 注入回归、facade catalog/Responses index workflow 回归，以及
  Responses JSON/SSE persistence-record 回归；
- `check_controller_services.py` 扩展为要求 `AiApiController` 只绑定 `IAiApiUseCase`，禁止遗留
  provider/session/index/gate/executor collaborator 回流；`check_lifecycle_services.py` 同时冻结
  facade 的 AppContext ownership、runtime wiring 与 `setUseCase(nullptr)` rollback/shutdown 顺序；
- Debug clean configure/build 通过；严格注册 374/374，`ctest` 374/374 通过（直接 test runner：
  374 cases / 1934 assertions）；
- architecture audit、cycle/layer、strict test registration、source ownership、include、
  target DAG、enqueue、AppContext、deadline、P5-W1/W2/W3 增量门禁通过；
- `src/domain` 保持无 JsonCpp 出边。

## 退出与后续

- `AccountController`、`ChannelController`、`HealthController`、`MetricsController` 与
  `RetoolWorkspaceController` 已各自只依赖一个 controller-facing use case；Account 的
  catalog/commands/store/backup/executor 编排收束进 `application/account/AccountAdminUseCase`；
- `AiApiController` 只依赖 `IAiApiUseCase`；具体 `AiApiUseCase` 是唯一允许协调 ProviderRegistry、
  chatSession/ResponseIndex/ExecutionGate、channel catalog、executor 和遗留 `GenerationService` 的
  composition seam。它同时拥有 provider model catalog 与 Responses response-index read/delete/persist
  workflow，Controller 不再直接触碰这些业务协作者；
- Workspace provision 已删除内部 Manager/Queue locator，持久化 store 由 runtime 注入；
- AccountManager 的 Config/Channel/Workspace/Provision 依赖已全部改为注入，新增
  `check_account_services.py` 防止 Account application 路径恢复 locator；
- managedAccount 与 Retool Provider 的业务 locator inventory 已归零；
- session application 路径已新增 `ISessionPersistence/ITelemetrySink` 并复用
  `IChannelCatalog`：ResponseIndex/chatSession/RequestAdapters/GenerationService/telemetry helper
  的 locator 与具体服务 include 已归零；
- 新增 `check_session_application_services.py`，并将 `sessionManager` 分层 allowlist 收紧，
  删除 accountManager/channelManager/dbManager/metrics 四类出边；
- 新增 `check_lifecycle_services.py`，禁止恢复 `chatSession`/`chaynsThreadReaper` singleton，
  并检查 AppContext ownership、Workspace concrete store/facade、ledger/metrics 构造注入与
  deadline-aware owner 接线；
- `chatSession`、Reaper、BackgroundTaskQueue、SessionDbManager、thread ledger 与 metrics
  worker/store，以及 AccountManager、ChannelManager、五个 concrete DB store
  （Account/AccountBackup/Channel/Config/RetoolWorkspace）、Workspace manager/service 的生命周期均已迁移；
- P5-W3 已满足“Controller 只依赖 use case”的全量退出标准，可标记 `DONE`。P6-W1 从 Result/Error/
  ProviderBase contract 开始；P7 再在保持 `IAiApiUseCase` 边界的前提下拆解 facade 内的遗留
  `GenerationService` pipeline，不能把该协作重新泄漏回 Controller。

## 回滚

端口、runtime 接线、application use case、Controller 调用和门禁必须成组回滚；禁止只恢复
Controller 的 singleton fallback，否则会形成新旧双轨并绕过测试 Fake。
