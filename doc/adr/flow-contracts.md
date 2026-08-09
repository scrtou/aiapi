# 业务流程调用契约

本文描述重构后的每条业务流程中，类的直接调用关系、线程归属、失败传播和取消语义。

## 1. 启动流程

```text
main
 └─ ConfigLoader::load
    └─ ConfigValidator::validate
       └─ InfrastructureBuilder::build
          ├─ DrogonDbPool::open
          ├─ *StoreSql::ensureSchema
          ├─ ProviderRegistry::registerProductionProvider
          ├─ BoundedExecutor::start
          ├─ AccountWorkerSupervisor::start
          ├─ SessionJanitor::start
          └─ ThreadReaper::start
             └─ RouteRegistrar::register
                └─ drogon::app().run
```

线程：配置和构造在 main；worker/timer 启动后独立运行。任何必需依赖失败返回 `Result` 并且不启动
后续线程；可选 DB 降级必须有明确配置和 readiness 指标。

## 2. Chat Completions 非流式

```text
AiController::chat
 ├─ ChatRequestCodec::decode
 ├─ ChatCompletionUseCase::execute
 │  └─ BoundedExecutor::enqueue
 │     └─ GenerationPipeline::run
 │        ├─ RequestValidationStage::validate
 │        ├─ ContinuityStage::resolve
 │        ├─ SessionLoadStage::loadOrCreate
 │        ├─ ExecutionGateStage::acquire
 │        ├─ ProviderInvocationStage::invoke
 │        │  └─ ProviderRegistry::find → Chayns/RetoolProvider::generate
 │        ├─ ToolBridgeStage（按能力启用）
 │        ├─ OutputPolicyStage::sanitize/normalize/validate
 │        ├─ EventPublicationStage::publish
 │        │  └─ ChatJsonSink::onEvent
 │        ├─ SessionCommitStage::commit
 │        └─ ExecutionGateStage::release
 └─ ChatJsonSink::onClose → IoLoopResponseStream/HTTP callback
```

错误在 application 中保持 `Result/Error`；只有 Controller 将 Error 映射为 HTTP 状态码。队列满返回
503；会话冲突返回 409；上游错误返回 502/429/504；客户端断连触发取消并停止后续事件。

## 3. Chat Completions 流式

调用链与非流式相同，仅将 sink 替换为 `ChatSseSink`，所有 `onEvent` 通过串行
`IoLoopResponseStream` 回到 Drogon loop。Provider 不得直接调用 Drogon response stream。

## 4. Responses API

```text
AiController::responsesCreate
 ├─ ResponsesRequestCodec::decode
 ├─ ResponsesUseCase::create
 │  └─ GenerationPipeline::run（与 Chat 共用）
 │     ├─ ContinuityStage::resolve(previous_response_id)
 │     ├─ ResponseIndex::resolve/bind
 │     ├─ ProviderInvocationStage
 │     └─ SessionCommitStage::commit + ResponseIndex::store
 └─ ResponsesJsonSink / ResponsesSseSink
```

`GET /responses/{id}` 只经过 `ResponsesUseCase::get` 和 `IResponseIndex`，不重新调用 Provider；
`DELETE` 只执行 index/session 删除策略。

## 5. Chayns Provider

```text
ChaynsProvider::doGenerate
 ├─ ChaynsModelCatalog::find
 ├─ ChaynsAccountSelector::select
 ├─ ChaynsThreadSession::loadOrCreate
 ├─ ChaynsHttpClient::uploadImage（可选）
 ├─ ChaynsHttpClient::postMessage
 ├─ ChaynsPollingLoop::runUntilComplete
 │  └─ ChaynsMessageCorrelation::correlate/deduplicate
 ├─ RetryPolicy/AccountSelector（失败时换账号）
 └─ ChaynsThreadLedger::touch/upsert
```

所有 HTTP/轮询都使用同一个 `ProviderCallContext.deadline` 和 `CancellationToken`。POST 结果不明确时
禁止盲目重发；只有明确拒绝或最终错误才允许切换账号。

## 6. Retool Provider

```text
RetoolProvider::doGenerate
 ├─ RetoolWorkspaceSelector::select/pin
 ├─ WorkspaceStore::get
 ├─ if workflow → RetoolWorkflowClient::execute
 ├─ if agent    → RetoolAgentClient::execute
 ├─ RetoolTemplateCodec::patch/parse
 └─ RetoolWorkspaceSelector::release
```

workflow 和 agent 的 HTTP、模板和响应状态机不得为了复用而塞进 Chayns 抽象；共享的只有
ProviderBase 边界、错误模型、deadline/cancellation 和 metrics decorator。

## 7. 账号管理

```text
AccountController
 └─ AccountCommandService::add/update/remove
    ├─ AccountStateMachine::transition
    ├─ AccountSelector/AccountCatalog（内存纯逻辑）
    ├─ IAccountStore::transactionalWrite
    ├─ IAccountClient::validate/refresh/delete（可选）
    └─ MetricsSink::record
```

后台由 `AccountWorkerSupervisor` 管理 `TokenRefreshWorkflow`、`RegistrationWorkflow` 和过期清理；
worker 只向 `BoundedExecutor` 投递有界任务，停机时先停止生产再 drain。

## 8. Chayns thread 回收

```text
ThreadReaper::runOnce
 ├─ IThreadLedger::claimBatch
 ├─ ChaynsThreadAdmin::delete
 ├─ success → IThreadLedger::remove
 └─ failure → IThreadLedger::retry/deadLetter
```

删除请求必须携带台账中的账号、origin、referer；只有上游确认成功才删除台账记录。

## 9. 停机流程

```text
ShutdownCoordinator::shutdown(deadline)
 ├─ RouteRegistrar::stopAccepting
 ├─ CancellationSource::cancelAll
 ├─ AccountWorkerSupervisor::requestStop
 ├─ SessionJanitor::requestStop
 ├─ ThreadReaper::requestStop
 ├─ BoundedExecutor::beginDraining
 ├─ waitUntil(deadline)
 ├─ join workers
 └─ DrogonDbPool/HTTP clients close
```

C++17 没有带超时的 `thread::join`。deadline 到期仍有活动线程时，必须进入显式、可观测的进程级
强制退出路径，禁止 detach 后继续析构被线程访问的对象。
