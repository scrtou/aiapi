# 目标模块类目录与接口责任

本文给出每个目标模块的主要类、公开方法和直接调用者。方法名是 C++17 草案，具体类型以
`interface-drafts.md` 和 ADR 为准。任何新类必须能在此表中找到唯一 owner。

## 1. platform/domain

| 类 | 主要方法 | 被谁调用 |
|---|---|---|
| `Result<T>` | `success`, `failure`, `ok`, `value`, `error` | 所有跨层 use case/adapter |
| `CancellationSource/Token` | `cancel`, `token`, `isCancelled`, `registerCallback` | Runtime、Executor、ProviderBase、Sink |
| `Deadline` | `expired`, `remaining` | Application pipeline、HTTP adapter |
| `ContinuityPolicy` | `resolve`, `nextSessionId` | `ContinuityStage` |
| `AccountSelector` | `eligible`, `choose`, `markAttempted` | `AccountCommandService`、ChaynsProvider |
| `AccountStateMachine` | `transition`, `canTransition` | AccountLifecycle、RegistrationWorkflow |
| `IChatProvider` | `generate`, `capabilities` | `ProviderInvocationStage` |
| `IModelCatalog` | `list`, `refresh`, `find` | Model use case、ProviderInvocationStage |
| `ISessionStore` | `load`, `create`, `update`, `remove`, `listExpired` | SessionLoad/Commit、SessionJanitor |
| `IResponseIndex` | `bind`, `resolve`, `store`, `erase`, `cleanup` | Responses use case、SessionCommit |
| `IExecutionGate` | `tryAcquire`, `cancelPrevious`, `release` | ExecutionGateStage |
| `IAccountStore` | `list`, `insert`, `update`, `remove`, `claimWaiting` | Account services |
| `IWorkspaceStore` | `list`, `get`, `upsert`, `updateUsage`, `disable` | Workspace services、Retool selector |
| `IThreadLedger` | `claimBatch`, `upsert`, `touch`, `remove`, `deadLetter` | ThreadReaper、ChaynsProvider |

## 2. application/generation

| 类 | 方法 | 直接调用顺序 |
|---|---|---|
| `ChatCompletionUseCase` | `execute(GenerationCommand, EventSink&)` | `AiController` → `GenerationPipeline` |
| `ResponsesUseCase` | `create`, `get`, `delete` | `AiController`；GET/DELETE 不经过生成 pipeline |
| `GenerationPipeline` | `run`, `runStage` | 编排以下 stage，不访问具体 Provider |
| `RequestValidationStage` | `validate` | Pipeline 第 1 步 |
| `ContinuityStage` | `resolve` | 根据 previous_response_id/zero-width/hash 生成稳定 key |
| `SessionLoadStage` | `loadOrCreate` | 读写 `ISessionStore`，返回 immutable execution snapshot |
| `ExecutionGateStage` | `acquire`, `release` | Provider 调用前后包裹；冲突返回 409 |
| `ProviderInvocationStage` | `invoke` | Registry → ProviderBase → ProviderResponse |
| `ToolBridgeStage` | `prepareRequest`, `parseResponse` | 仅在 provider 不支持原生 tools 时启用 |
| `OutputPolicyStage` | `sanitize`, `normalize`, `validate` | ProviderResponse → ClientEvents |
| `EventPublicationStage` | `publish`, `close` | 调用 `GenerationEventSink`，处理断连取消 |
| `SessionCommitStage` | `commit`, `bindResponse` | 发送终结事件后写 SessionStore/ResponseIndex |

## 3. application/account/workspace/thread

| 类 | 方法 | 直接调用者 |
|---|---|---|
| `AccountQueryService` | `list`, `get`, `backupList` | AccountController、Health |
| `AccountCommandService` | `add`, `update`, `remove`, `refresh` | AccountController |
| `AccountLifecycle` | `activate`, `disable`, `rollback`, `expire` | Command/Registration workflow |
| `TokenRefreshWorkflow` | `refreshAll`, `refreshOne`, `check` | AccountWorkerSupervisor |
| `RegistrationWorkflow` | `register`, `rollback` | AccountCommandService、定时 worker |
| `AccountWorkerSupervisor` | `start`, `stop`, `waitUntil` | AppContext/ShutdownCoordinator |
| `ChannelService` | `list`, `create`, `update`, `remove`, `setStatus` | ChannelController、AccountSelector |
| `WorkspaceService` | `list`, `get`, `upsert`, `disable`, `updateUsage` | WorkspaceController、RetoolWorkspaceSelector |
| `WorkspaceProvisioningWorkflow` | `provision`, `verify`, `rollback` | WorkspaceController/后台 executor |
| `ThreadReaper` | `start`, `requestStop`, `runOnce`, `waitUntil` | Runtime timer、ShutdownCoordinator |
| `ReadinessService` | `live`, `ready`, `dependencies` | HealthController |
| `MetricsQueryService` | `errors`, `status`, `summary` | MetricsController |

## 4. infrastructure/provider

| 类 | 方法 | 责任 |
|---|---|---|
| `ProviderBase` | `generate(final)`, `providerName`, `doGenerate` | NVI 公共边界；不实现协议流程 |
| `ProviderRegistry` | `register`, `find`, `list`, `disable` | 构造期注册，发布后只读快照 |
| `ChaynsProvider` | `doGenerate`, `capabilities` | 组合 account selector、thread session、polling loop |
| `ChaynsHttpClient` | `createThread`, `postMessage`, `poll`, `uploadImage`, `deleteThread` | 只做 HTTP 和 wire codec |
| `ChaynsThreadSession` | `load`, `create`, `send`, `commit`, `transfer` | thread 生命周期和 correlation key |
| `ChaynsPollingLoop` | `runUntilComplete`, `waitNext` | 可取消轮询和退避 |
| `RetoolProvider` | `doGenerate`, `capabilities` | workflow/agent 分支选择和 workspace usage |
| `RetoolWorkflowClient` | `execute`, `parse` | Workflow wire 协议 |
| `RetoolAgentClient` | `createThread`, `execute`, `parse` | Agent wire 协议 |
| `RetoolWorkspaceSelector` | `select`, `pin`, `release` | 亲和、健康和 in-use 计数 |
| `ProviderErrorMapper` | `fromHttp`, `fromBody`, `toMetric` | transport error → domain Error |

## 5. infrastructure/persistence/executor/transport/runtime

| 类 | 方法 | 责任 |
|---|---|---|
| `SessionStoreSql` | `load`, `upsert`, `remove`, `listExpired` | SQL 和 snapshot codec；不含连续性规则 |
| `ResponseIndexSql` | `bind`, `resolve`, `store`, `erase`, `cleanup` | response_id 映射持久化 |
| `BoundedExecutor` | `start`, `enqueue`, `beginDraining`, `waitUntil`, `join` | 四态、有界队列和 worker 异常隔离 |
| `ChatRequestCodec` | `decode`, `validateSyntax` | HTTP JSON → ChatCommand |
| `ResponsesRequestCodec` | `decode`, `validateSyntax` | HTTP JSON → ResponsesCommand |
| `Chat/Responses*Sink` | `onEvent`, `onClose`, `isValid` | 语义事件 → OpenAI JSON/SSE；不执行业务 |
| `AppContext` | `build`, `run`, `shutdown` | 唯一组合根和生命周期 owner |
| `InfrastructureBuilder` | `buildStores`, `buildProviders`, `buildWorkers` | 依赖构造和失败回滚 |
| `RouteRegistrar` | `registerControllers`, `registerFilters`, `registerCors` | Drogon wiring |
| `ShutdownCoordinator` | `stopAccepting`, `broadcastCancel`, `drain`, `wait`, `join` | 统一绝对 deadline 停机 |
