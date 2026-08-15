# 目标代码结构与依赖组织

> 本文定义重构完成后的**目标文件结构、target DAG 和对象所有权**。它不是要求一次性
> 创建所有文件；每个文件只有在对应迁移工作包通过门禁后才落地。接口草案见
> [`interface-drafts.md`](./interface-drafts.md)，当前源码事实见
> [`source-audit-2026-08.md`](../audits/source-audit-2026-08.md)。

## 1. 顶层依赖图

箭头指向被依赖 target。P8 后不存在 `aiapi_legacy` 或反向内部 link：

```text
aiapi_application ──> aiapi_domain ──> aiapi_platform
aiapi_infrastructure ─┬─> aiapi_domain
                       └─> aiapi_platform
aiapi_transport ──────┬─> aiapi_application
                       ├─> aiapi_domain
                       └─> aiapi_platform
aiapi_runtime ────────┬─> aiapi_application
                       ├─> aiapi_infrastructure
                       └─> aiapi_transport
aiapi (main.cc) ──────> aiapi_runtime
```

实际 CMake DAG（`aiapi_transport` 还以 whole-archive 形式进入最终可执行文件，以保留
Drogon 的静态 Controller 注册对象）：

```text
aiapi_platform
└── aiapi_domain
    └── aiapi_application

aiapi_infrastructure ──> aiapi_domain, aiapi_platform, Drogon/OpenSSL/PostgreSQL
aiapi_transport      ──> aiapi_application, aiapi_domain, aiapi_platform, Drogon
aiapi_runtime        ──> aiapi_application, aiapi_infrastructure, aiapi_transport
aiapi (main.cc)      ──> aiapi_runtime (+ whole-archive aiapi_transport)
```

约束：

- `domain` 只依赖标准库和 `aiapi_platform`；禁止 JsonCpp、Drogon、DB、OpenSSL。
- `application` 通过 domain port 编排；可在 request/event 值边界使用 JsonCpp，但禁止 include
  concrete Provider、DbManager 或 Drogon。
- `infrastructure` 实现 port，可依赖第三方库；不得被 domain 反向 include。
- `transport` 只做 HTTP/SSE/JSON 映射和 use-case 调度，不持有业务状态。
- `runtime` 是唯一组合根，负责构造、注入、启动和停机。

## 2. 目标文件树

```text
src/
├── platform/
│   ├── result/Result.h Error.h ErrorCode.h
│   ├── time/{Clock.h,SystemClock.cpp,Deadline.h}
│   ├── concurrency/{Cancellation.h,Executor.h,Executor.cpp}
│   ├── logging/{Logger.h,MetricsContext.h}
│   └── types/{JsonDocument.h,StrongIds.h}
├── domain/
│   ├── conversation/
│   │   ├── model/{Conversation.h,Message.h,ToolDefinition.h,Image.h,Usage.h}
│   │   ├── policy/{ContinuityPolicy.h,HistoryBudget.h,OutputPolicy.h}
│   │   └── port/{ISessionStore.h,IResponseIndex.h,IExecutionGate.h}
│   ├── provider/
│   │   ├── model/{ProviderRequest.h,ProviderResponse.h,ProviderCapabilities.h}
│   │   └── port/{IChatProvider.h,IModelCatalog.h,IProviderRegistry.h}
│   ├── account/
│   │   ├── model/{Account.h,AccountCredential.h,AccountStatus.h,AccountRequirement.h}
│   │   ├── policy/{AccountSelector.h,AccountStateMachine.h}
│   │   └── port/{IAccountStore.h,IAccountClient.h}
│   ├── channel/{Channel.h,ChannelPolicy.h,port/IChannelStore.h}
│   ├── workspace/{Workspace.h,WorkspaceStatus.h,port/IWorkspaceStore.h}
│   ├── thread/{ThreadLedgerEntry.h,port/IThreadLedger.h,IThreadAdmin.h}
│   ├── metrics/{ErrorEvent.h,MetricSink.h}
│   └── port/{IConfigStore.h,IClock.h,ILogger.h}
├── application/
│   ├── generation/
│   │   ├── ChatCompletionUseCase.{h,cpp}
│   │   ├── ResponsesUseCase.{h,cpp}
│   │   ├── GenerationPipeline.{h,cpp}
│   │   ├── GenerationContext.h
│   │   └── stages/
│   │       ├── RequestValidationStage.{h,cpp}
│   │       ├── ContinuityStage.{h,cpp}
│   │       ├── SessionLoadStage.{h,cpp}
│   │       ├── ExecutionGateStage.{h,cpp}
│   │       ├── ProviderInvocationStage.{h,cpp}
│   │       ├── ToolBridgeStage.{h,cpp}
│   │       ├── OutputPolicyStage.{h,cpp}
│   │       ├── EventPublicationStage.{h,cpp}
│   │       └── SessionCommitStage.{h,cpp}
│   ├── account/
│   │   ├── AccountQueryService.{h,cpp}
│   │   ├── AccountCommandService.{h,cpp}
│   │   ├── AccountLifecycle.{h,cpp}
│   │   ├── TokenRefreshWorkflow.{h,cpp}
│   │   ├── RegistrationWorkflow.{h,cpp}
│   │   └── AccountWorkerSupervisor.{h,cpp}
│   ├── channel/ChannelService.{h,cpp}
│   ├── workspace/{WorkspaceService.h,WorkspaceProvisioningWorkflow.h}
│   ├── thread/ThreadReaper.{h,cpp}
│   ├── health/ReadinessService.{h,cpp}
│   └── metrics/MetricsQueryService.{h,cpp}
├── infrastructure/
│   ├── provider/
│   │   ├── ProviderBase.{h,cpp}
│   │   ├── ProviderRegistry.{h,cpp}
│   │   ├── chayns/
│   │   │   ├── ChaynsProvider.{h,cpp}
│   │   │   ├── ChaynsHttpClient.{h,cpp}
│   │   │   ├── ChaynsThreadSession.{h,cpp}
│   │   │   ├── ChaynsPollingLoop.{h,cpp}
│   │   │   ├── ChaynsAccountSelector.{h,cpp}
│   │   │   ├── ChaynsModelCatalog.{h,cpp}
│   │   │   └── ChaynsThreadAdmin.{h,cpp}
│   │   └── retool/
│   │       ├── RetoolProvider.{h,cpp}
│   │       ├── RetoolHttpClient.{h,cpp}
│   │       ├── RetoolWorkspaceSelector.{h,cpp}
│   │       ├── RetoolWorkflowClient.{h,cpp}
│   │       ├── RetoolAgentClient.{h,cpp}
│   │       └── RetoolTemplateCodec.{h,cpp}
│   ├── persistence/
│   │   ├── DrogonDbPool.{h,cpp}
│   │   ├── stores/{AccountStore,ChannelStore,SessionStore,ResponseIndexStore,
│   │   │           WorkspaceStore,ConfigStore,ThreadLedgerStore,MetricsStore}.{h,cpp}
│   │   └── migrations/{ProviderRetirement,SchemaMigrator}.{h,cpp}
│   ├── codec/{HttpJsonCodec,ProviderJsonCodec,SessionSnapshotCodec,DbRowCodec}.{h,cpp}
│   ├── executor/BoundedExecutor.{h,cpp}
│   ├── metrics/{MetricsWorker,ErrorStatsSink,StatusSink}.{h,cpp}
│   └── config/{ConfigLoader,ConfigValidator,ProcessConfig}.{h,cpp}
├── transport/
│   ├── controllers/{AiController,AccountController,ChannelController,
│   │                 WorkspaceController,HealthController,MetricsController,LogController}.{h,cpp}
│   ├── codec/{ChatRequestCodec,ResponsesRequestCodec,ChatResponseCodec,ResponsesResponseCodec}.{h,cpp}
│   ├── sinks/{ChatJsonSink,ChatSseSink,ResponsesJsonSink,ResponsesSseSink}.{h,cpp}
│   └── filters/{AdminAuthFilter,RateLimitFilter,CorsAdvice}.{h,cpp}
├── runtime/
│   ├── AppContext.{h,cpp}
│   ├── InfrastructureBuilder.{h,cpp}
│   ├── RouteRegistrar.{h,cpp}
│   ├── ShutdownCoordinator.{h,cpp}
│   └── main.cc
└── test/
    ├── unit/（domain/application 各 port fake）
    ├── contract/（Provider/HTTP/SSE fixture）
    └── integration/（启动、停机、数据库、Drogon）
```

## 3. 对象所有权

`AppContext` 持有所有有状态对象：`BoundedExecutor`、`SessionStore`、`ResponseIndex`、
`AccountCatalog`、`WorkspaceCatalog`、`ProviderRegistry`、`MetricsWorker`、`ThreadReaper`。
Controller、Provider 和 codec 均为构造注入的借用引用或 `shared_ptr`，不得自行创建全局对象。

停机顺序固定为：停止接收 → 广播取消 → 停止 timer/reaper → executor Draining → 等待统一 deadline
→ join → 关闭 DB/HTTP。
