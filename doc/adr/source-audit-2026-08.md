# `src/` 全量架构审计（2026-08）

> 本文是对当前工作区源码的事实审计，不是目标架构。目标架构和不可变决策见
> [`RFC-001-architecture-refactor.md`](./RFC-001-architecture-refactor.md)，施工顺序见
> [`migration-plan.md`](./migration-plan.md)。审计基于 2026-08-09 工作区；其中
> `ChaynsPollingPolicy.h` 和 `chaynsapi.cpp` 有用户未提交修改，审计不覆盖/回滚这些修改。

## 1. 审计方法与结论

执行了以下检查并对主要实现逐文件阅读：

```text
find src -type f
wc -l（按模块和文件）
rg 单例、线程、sleep_for、同步 HTTP/DB、APIinterface 方法调用
architecture_audit.py、check_cycles.py、layer-rules.json
```

当前生产代码约 160 个头/源文件、36,221 行、65 个 production translation unit；测试为
35 个源文件、223 个 DROGON_TEST。结构门禁和测试目前通过，但这只证明“现有行为可编译/可运行”，
不证明边界、取消和停机安全。

**总判断：现有 RFC 方向正确，但尚未达到可直接施工的粒度。必须增加“按所有权和流程拆分”的
实施层，并把以下四项列为重构前置条件：**

1. `APIinterface` 的生成、模型目录、生命周期、线程映射和 session 副作用必须拆成独立 port；
2. `chatSession`、`GenerationService`、`AccountManager` 和两个活跃 Provider 需要先做行为锁定，再按
   IO/策略/状态所有权重写；
3. 测试 CMake 不能继续复制生产源；否则重构时会出现“测试通过但运行 target 未链接新代码”；
4. 所有队列、HTTP、轮询和 DB 写入必须登记绝对 deadline、取消语义和失败处理。

## 2. 文件与模块审计

### 2.1 domain（13 文件，1,093 行）

| 文件组 | 当前职责 | 问题 | 目标归属 |
|---|---|---|---|
| `model/SessionData.h` | `session_st`、请求/响应/Provider 上下文 | 领域模型包含 `Json::Value`、协议字段和上游桥接状态；仍是可变大对象 | `domain::conversation` 的值对象；JSON 放 `transport/codec`，桥接状态放 application |
| `model/AccountData.h` | 账号字段、JSON codec、排序比较器 | JsonCpp 与密码/token 同一公开结构；比较器是可变堆策略 | `Account`、`AccountCredential`、`AccountPoolKey`；脱敏 codec 和 selector 分开 |
| `model/ChannelInfo.h` | 渠道字段及 JSON codec | DB snake_case 与领域值混合 | `Channel` 值对象；HTTP/DB codec 外置 |
| `model/ProviderResult.h` | Provider 结果、错误、usage、tool calls | `Json::Value meta` 及 HTTP 状态码把 transport 语义带入 domain | `ProviderResponse`、`ProviderError`；状态码映射留 adapter |
| `model/RetoolWorkspaceInfo.h` | Retool 凭据、状态、JSON codec | 密钥、Cookie、持久化 JSON 混在同一 DTO | `Workspace` + `WorkspaceSecret`；持久化/公开视图分离 |
| `model/ErrorEvent.h` | 指标事件和 JSON 详情 | 观测模型依赖 JsonCpp；业务和观测维度耦合 | `metrics::ErrorEvent`（纯字段/Detail map）+ metrics codec |
| `model/ImageInfo.h`、`BridgeWireFormat.h` | 多模态图片和桥格式枚举 | 图片字段仍允许上传结果和协议细节 | 图片输入值对象；上传结果为 provider adapter 私有类型 |
| `port/APIinterface.h` | 8 个无关职责的虚函数和公共可变模型表 | 端口过宽；所有 Provider 被迫实现空方法；生成传入 `session_st&` | 删除；拆为 `IChatProvider`、`IModelCatalog`、`IProviderLifecycle`、`IThreadContext` |
| 其它 port | Account/Channel/Config/Retool store | 已有端口可用，但签名仍带 JSON/可变 DTO，缺 Result/取消 | 逐步改为 `Result`、不可变值对象和事务端口 |

**硬门禁：** domain 只能依赖标准库和 `platform`。因此 `JsonCpp`、Drogon、OpenSSL、DB 类型必须在
本阶段通过 codec/adapter 移出，不能以“模型文件”例外长期保留。

### 2.2 sessionManager（49 文件，13,452 行）

| 文件 | 事实职责 | 重构动作 |
|---|---|---|
| `core/Session.h/.cpp` | 内存会话表、Hash/ZeroWidth/Responses 连续性、TTL 清理、DB 写穿、线程映射通知 | 重写为 `SessionStore`（状态）+ `ContinuityService`（决策）+ `SessionPersistence`（adapter）+ `SessionJanitor`（生命周期）；禁止静态实例 |
| `core/SessionCodec.*` | `session_st` JSON 快照 | 移到 infrastructure persistence codec；只接受纯 domain snapshot |
| `core/GenerationService.*`、`GenerationServiceEmitAndToolBridge.cpp` | 门控、provider 调用、重试、工具桥接、清洗、事件发射、指标和会话提交 | 拆成 application pipeline stages：`RequestMaterializer`、`ExecutionCoordinator`、`ToolBridgeStage`、`OutputPolicy`、`EventPublisher`、`SessionCommitter`；单个 stage 不得访问单例 |
| `core/RequestAdapters.*` | HTTP 读取、Chat/Responses JSON 解析、图片/工具/零宽字符处理 | HTTP codec 只做语法；业务校验移 `RequestValidator`；连续性输入由 `ContinuityInput` 表示 |
| `core/SessionExecutionGate.h` | 全局静态会话锁和 CancelPrevious | 注入 `IExecutionGate`；token 与 deadline 合并，提供可观测 AcquireResult |
| `core/ClientOutputSanitizer.*` | 客户端兼容规则和文本清洗 | 纯 `OutputPolicy`，不依赖 Drogon/metrics |
| `continuity/*` | 零宽/Hash/previous_response_id 解析、budget、ResponseIndex | Resolver/Index 分离；Index 持久化由 port 注入；budget 保持纯函数 |
| `tooling/*` | 工具定义编码、XML/JSON bridge、校验、自愈、严格客户端规则 | 已是较好的策略边界；移除对 `ErrorStatsService` 单例和 Drogon 的直接依赖，组合进 pipeline |
| `actionProtocol/*` | 客户端能力识别和协议编译 | 保持 domain policy；JSON codec 放 edge |
| `contracts/*` | GenerationRequest/Event/Sink | Request/Event 改为纯 C++；`Json::Value` 字段改 `JsonDocument`（edge type）或结构化值；Sink 留 transport |

### 2.3 apipoint（15 文件，5,680 行）

| Provider/文件 | 已确认的真实流程 | 必须保留的差异 | 目标拆分 |
|---|---|---|---|
| `chaynsapi.cpp`（1,441 行） | 账号选择→单账号 gate→personId→图片上传→thread create/message→轮询→消息关联→重试/换账号→thread 台账 | JSON thread/message/polling、Pro/Free 路由、重复/乱序消息处理 | `ChaynsProvider : ProviderBase`；`ChaynsHttpClient`、`ChaynsThreadSession`、`ChaynsPollingLoop`、`ChaynsAccountSelector`、`ChaynsThreadLedger` 组合 |
| `ChaynsModelCatalog.*` | 模型刷新、能力和图片 MIME 检查 | catalog TTL/强制刷新 | 独立 `IModelCatalog`，Provider 只读快照 |
| `ChaynsMessageCorrelation.*` | 解析 assistant/user message 关联 | 纯解析规则 | domain policy + fixture |
| `ChaynsPollingPolicy.h` | 退避、deadline 和错误分类 | 上游协议策略 | 保持纯 policy；注入 Clock/Cancellation |
| `chaynsThreadReaper.*` | timer/worker 扫描台账、HTTP DELETE、重试和最终清理 | 资源回收是独立生命周期 | `ThreadReaper` use case + `IThreadLedger` + `IChaynsThreadAdmin` |
| `retoolapi.cpp`（1,352 行） | workspace 亲和/选池→workflow 或 agent 分支→模板 patch→HTTP JSON→解析/错误分类→usage | workflow/agent 两套 wire 协议、Cookie/XSRF、workspace usage | `RetoolProvider : ProviderBase`；`RetoolWorkflowClient`、`RetoolAgentClient`、`WorkspaceSelector`、`RetoolTemplateCodec` |
| `nexosapi.*`（1,335 行） | 登录、cookie/session、chat.data 解码、账号配额/预算、DB 备份和删除 | 目标退役，不再作为重构样板 | 先 tombstone/归档，再删除全部实现和注册 |
| `openai/OpenAiProvider.*` | 旧 OpenAI 直连实现 | 目标退役；公开协议不退役 | 同 nexos，禁止新代码依赖 |
| `ApiFactory.*`、`ApiManager.*` | 静态注册、void* 工厂、按模型 priority_queue、启停 | 工厂线程安全和模型选择未定义；空队列 `top()` 风险；Provider 接口泄漏 | `ProviderRegistry`（构造期显式注册）+ `ProviderRouter`（只读快照）+ `IModelCatalog` |

### 2.4 controllers 与 sinks（25 文件，4,245 行）

- `AiApiController.cc` 同时做路由推断、JSON 解析、队列提交、GenerationService 构造、ResponseIndex 写入、SSE/JSON 响应；应只保留 transport mapping，依赖注入 `ChatCompletionUseCase`/`ResponsesUseCase`。
- `AccountController.cc` 直接操作 AccountManager、AccountDbManager、BackgroundTaskQueue，且 enqueue 返回值在多数分支被忽略；改为 command/query use case，统一 `EnqueueResult` 错误映射。
- `ChannelController.cc`、`RetoolWorkspaceController.cc` 同样直接访问 manager/singleton；Workspace Controller 还直接读 ConfigDbManager。
- `HealthController.cc` 通过单例探针判断 ready；改为注入 `ReadinessProbe`，区分 liveness、readiness、degraded。
- `MetricsController.cc` 直接依赖多个 DB manager；改为 `MetricsQueryService` 和只读 port。
- `AdminAuthFilter.h`、`RateLimitFilter.h`、`ControllerUtils.h` 是 transport 横切组件；RateLimit 状态必须由 AppContext 拥有。
- 四个 sink（`Chat*Sink`、`Responses*Sink`）假定所有事件在同一 worker 线程到达，`closed_` 非原子；保持“单线程 actor”契约或加串行 dispatcher，禁止跨线程直接调用。

### 2.5 dbManager（19 文件，4,638 行）

所有 manager 都是 singleton 并直接持有 Drogon `DbClient`。`SessionDbManager`、`chaynsThreadDbManager` 的 async 方法直接依赖全局 `BackgroundTaskQueue`，且调用点忽略 `bool` 返回，停机时可能丢任务。

按领域拆分为独立 store adapter：`AccountStore`、`AccountBackupStore`、`ChannelStore`、`SessionSnapshotStore`、`ResponseIndexStore`、`ChaynsThreadLedger`、`RetoolWorkspaceStore`、`ConfigStore`、`ErrorStatsStore`、`StatusStore`。SQL 方言和 JSON codec 留在 infrastructure；业务状态机不得进入 DB 类。

### 2.6 account/channel/workspace/managed/metrics/utils

- `accountManager.cpp` 2,766 行，混合账号池/排序、token HTTP、Nexos 注册/删除、Retool provision、配置持久化和四个后台线程；这是最高复杂度的重写对象。先抽纯 selector/state machine，再抽 workflow 和 workers。
- `channelManager.cpp` 既是内存缓存又负责默认渠道和 store 初始化；拆为 `ChannelCatalog`（纯状态）与 `ChannelService`（用例）。
- `RetoolWorkspaceManager/Service` 已有 port 试点，但仍是双 singleton；Service 的 provision 任务必须归 AppContext executor。
- `managedAccount/*` 是跨 Provider 账号抽象的雏形；保留 contract/backend，删除 backend 内的 singleton，通过构造注入。
- `ErrorStatsService` 自带 worker 并直接拿 DB singleton；改为 `MetricsSink` + `MetricsWorker`，错误事件只在 application 出口上报一次。
- `utils/BackgroundTaskQueue.h` 当前为无界 `std::queue`、`bool enqueue`、不可逆 shutdown 但无 deadline；按 ADR-08 改四态和容量背压。`IoLoopResponseStream` 必须是唯一 loop-affinity adapter。
- `apiManager/*` 的 `void*` 工厂和 priority queue 是 wiring 风险，必须在 Provider 迁移前替换。

### 2.7 `main.cc`（433 行）

当前同时承担配置解析/校验、CORS、反射注册、线程池启动、所有 store 注入、建表、reaper/timer、Drogon run 和停机。
目标拆成 `ConfigLoader`、`InfrastructureBuilder`、`AppContext`、`TransportRegistration`、`RuntimeLifecycle`、`ShutdownCoordinator`；`main.cc` 最终只保留约束检查、构造、run、shutdown 四步。

## 3. 流程级调用图与线程契约

### 3.1 启动

```text
ConfigLoader -> ConfigValidator -> InfrastructureBuilder
  -> DbPool/Stores -> ProviderRegistry -> AppContext
  -> workers/timers -> TransportRegistration -> Drogon::run
```

任何建表、provider 构造或 worker 启动失败都必须在 `AppContext::build()` 返回 `Result`；不能以 Null store 静默启动。

### 3.2 Chat/Responses 生成

```text
HTTP loop: Controller -> HTTP codec -> GenerationRequest
           -> Executor.enqueue (背压/取消)
worker:    ContinuityResolver -> SessionStore -> ExecutionGate
           -> ProviderBase::generate (deadline/token)
           -> ToolBridge/OutputPolicy -> EventPublisher -> Sink
           -> SessionCommitter/ResponseIndex
HTTP loop: IoLoopResponseStream -> callback
```

非流式和流式只在 sink adapter 分叉；两者共享同一 use case。Provider 不得写 `session.response`，
application 不得把 Drogon `HttpRequestPtr` 传入。

### 3.3 账号管理

```text
Controller -> AccountUseCase -> AccountState/Selector
           -> IAccountStore (事务) + ProviderAccountClient
           -> Executor (刷新/注册/清理) -> Metrics
```

账号选择必须是无 IO 的纯策略；token 刷新、注册、删除分别拥有 deadline、幂等键和补偿动作。

### 3.4 Chayns thread 回收

```text
Timer -> ThreadReaper -> IThreadLedger.claimBatch
      -> IChaynsThreadAdmin.delete (同账号+Origin)
      -> success: ledger.delete | failure: retry/backoff/dead-letter
```

`delete` 成功才允许删除台账；停机与聊天请求共享同一 cancellation/deadline。

### 3.5 停机

```text
SIGTERM -> stop accept -> broadcast cancel -> stop timers/reaper
        -> queue Draining -> wait_until(global_deadline)
        -> join workers -> close HTTP/DB -> exit
```

## 4. 风险登记

| 等级 | 风险 | 证据 | 必须动作 |
|---|---|---|---|
| P0 | Provider 端口过宽且写 session 副作用 | `APIinterface.h`、`GenerationService.cpp:467-492` | 先建 `IChatProvider`/ProviderBase adapter，再删旧口 |
| P0 | 测试复制生产源 | `src/test/CMakeLists.txt:66-108` | 先建共享 production library，删除 `PROJECT_SOURCES` |
| P0 | domain 依赖 JsonCpp | domain 五个 model + ErrorEvent | 先 codec 往返测试，再移除第三方类型 |
| P1 | 无界队列和 enqueue 失败丢失 | `BackgroundTaskQueue.h`、多处忽略返回值 | 四态/容量/统一错误事件 |
| P1 | 单例导致生命周期和线程不可控 | 277 行以上访问器命中 | AppContext 分阶段替换 |
| P1 | `Session` 1,257 行混合状态/DB/线程/协议 | `Session.cpp` | SessionStore/Janitor/Persistence 拆分 |
| P1 | AccountManager 2,766 行和 4 个 worker | `accountManager.cpp` | 纯策略先行，按 workflow 所有权重写 |
| P1 | GenerationService 2,746 行（含 emit 文件） | core 两个 cpp | pipeline stage 化，ProviderResult 只走 Result |
| P1 | 同步 HTTP/`sleep_for` 无统一取消 | chayns/retool/account/reaper | `ProviderCallContext` 和 CancellableWait |
| P2 | `ApiManager` priority_queue 空 top、无锁 | `ApiManager.cpp` | ProviderRouter 不可变快照 + 原子刷新 |
| P2 | Sink `closed_` 非原子 | 四个 sink | 明确单线程契约或串行 dispatcher |
| P2 | Null store 静默降级掩盖 wiring | account/channel/workspace manager | build 失败即返回；仅 DB 降级需显式配置 |
| P1 | 会话读写锁边界不一致 | `Session.cpp` 的 `updateSession` 先无锁查表后加锁；`getSession` 以 void 表示 miss | 所有 Store 操作内部一次加锁并返回 `Result<SessionSnapshot>` |
| P1 | 失败语义被 JSON 字段重建 | `GenerationService::executeProvider` 把 `ProviderResult` 再写入 `session.response.message` | 直接传播 `Result<ProviderResponse>`，transport 统一映射 |
| P2 | 线程安全依赖隐式约定 | Provider 的 thread/workspace map 和 sink 状态均靠调用顺序保护 | 为每个 map 定义 owner/executor，或封装 actor；TSan 测试 |

## 5. 目标 target 与所有权

最终 CMake target DAG：

```text
aiapi_platform      (std + logging/error/deadline)
aiapi_domain        (platform + pure models/policies/ports)
aiapi_application   (domain; use cases/pipeline)
aiapi_infrastructure(domain + platform + Drogon/DB/HTTP)
aiapi_transport     (application + infrastructure adapters + Drogon)
aiapi_runtime       (AppContext/lifecycle; links all)
aiapi               (main.cc + aiapi_runtime)
aiapi_test_*        (test target links production libraries; never source-copy)
```

依赖只能从左向右；`domain` 不可见 Drogon/JsonCpp。每个新 Provider target 必须通过编译期
`static_assert(std::is_base_of_v<ProviderBase, T>)` 注册；测试 fake 只实现 `IChatProvider`。

## 6. 需重写而不是继续打补丁的组件

以下组件已有多次半迁移痕迹，继续在原类上增加 wrapper 会形成永久双轨，应在行为测试后整体重写：

1. `APIinterface`/`ApiFactory`/`ApiManager`：删除 void* 和宽接口；
2. `chatSession`：以 `SessionStore`、`ContinuityService`、`SessionJanitor` 替换 singleton；
3. `GenerationService`：以显式 pipeline context 和 stage 组合替换跨文件大函数；
4. `AccountManager`：以 selector、account state machine、token workflow、registration workflow、worker supervisor 替换；
5. `BackgroundTaskQueue`：实现有界 executor 和统一停机；
6. 两个活跃 Provider：保留协议策略，重写 orchestration 外壳。

## 7. 审计后的方案调整

现有 RFC 的阶段顺序保留，但增加以下硬依赖：

- 阶段 1 必须包含 Provider/Account/Session/Shutdown 的真实运行时覆盖和假上游；
- 阶段 3 在 domain 净化前先完成测试 library 化；
- 阶段 4 队列重写必须先于 Provider 取消传播；
- 阶段 5 先迁移 `ApiManager`、`ResponseIndex`、`chatSession`，再迁移 Controller；
- 阶段 6 Provider 切片必须同时拆除 `afterResponseProcess/transferThreadContext/eraseChatinfoMap`；
- 阶段 7 只在 R1 同名竞争和旧 APIinterface 归零后进行；
- 每一阶段都要更新本审计的“事实状态”，并重新生成机器基线。

## 8. 完成标准

只有同时满足以下条件，才可将 RFC 标记为“已实施”：

1. CMake target DAG、include/layer/cycle 门禁全部通过；
2. domain 无第三方类型，application 无 Drogon/DB/单例；
3. 生成、账号、回收、停机四条流程均有线程/错误/取消契约测试；
4. 活跃 Provider 均继承 ProviderBase，且 `session.response` 写入为 0；
5. 测试只链接生产 library，覆盖报告证明修改路径实际执行；
6. nexos/OpenAiProvider 数据归档、恢复、410 tombstone 演练完成；
7. 正常 SIGTERM 全部 join，超时路径可观测且无 UAF。


## 附录 A：生产文件逐文件清单

以下清单由 `find src` 与行数脚本生成；“目标 owner”是重构后的归属，不表示当前代码已迁移。

| 文件 | 行数 | 目标 owner |
|---|---:|---|
| `src/accountManager/RetoolProvisionHealth.cpp` | 86 | application/account workflow |
| `src/accountManager/RetoolProvisionHealth.h` | 41 | application/account workflow |
| `src/accountManager/accountManager.cpp` | 2766 | application/account workflow |
| `src/accountManager/accountManager.h` | 170 | application/account workflow |
| `src/apiManager/ApiFactory.cpp` | 27 | runtime/provider registry |
| `src/apiManager/ApiFactory.h` | 39 | runtime/provider registry |
| `src/apiManager/ApiManager.cpp` | 115 | runtime/provider registry |
| `src/apiManager/ApiManager.h` | 53 | runtime/provider registry |
| `src/apiManager/Apicomn.h` | 14 | runtime/provider registry |
| `src/apipoint/chaynsapi/ChaynsMessageCorrelation.cpp` | 142 | infrastructure/provider (重构中) |
| `src/apipoint/chaynsapi/ChaynsMessageCorrelation.h` | 44 | infrastructure/provider (重构中) |
| `src/apipoint/chaynsapi/ChaynsModelCatalog.cpp` | 351 | infrastructure/provider (重构中) |
| `src/apipoint/chaynsapi/ChaynsModelCatalog.h` | 68 | infrastructure/provider (重构中) |
| `src/apipoint/chaynsapi/ChaynsPollingPolicy.h` | 45 | infrastructure/provider (重构中) |
| `src/apipoint/chaynsapi/chaynsThreadReaper.cpp` | 184 | infrastructure/provider (重构中) |
| `src/apipoint/chaynsapi/chaynsThreadReaper.h` | 66 | infrastructure/provider (重构中) |
| `src/apipoint/chaynsapi/chaynsapi.cpp` | 1441 | infrastructure/provider (重构中) |
| `src/apipoint/chaynsapi/chaynsapi.h` | 99 | infrastructure/provider (重构中) |
| `src/apipoint/nexosapi/nexosapi.cpp` | 1335 | infrastructure/provider (重构中) |
| `src/apipoint/nexosapi/nexosapi.h` | 106 | infrastructure/provider (重构中) |
| `src/apipoint/openai/OpenAiProvider.cpp` | 329 | infrastructure/provider (重构中) |
| `src/apipoint/openai/OpenAiProvider.h` | 45 | infrastructure/provider (重构中) |
| `src/apipoint/retoolapi/retoolapi.cpp` | 1352 | infrastructure/provider (重构中) |
| `src/apipoint/retoolapi/retoolapi.h` | 73 | infrastructure/provider (重构中) |
| `src/channelManager/channelManager.cpp` | 227 | application/channel catalog |
| `src/channelManager/channelManager.h` | 58 | application/channel catalog |
| `src/controllers/AccountController.cc` | 520 | transport/controller or sink |
| `src/controllers/AccountController.h` | 47 | transport/controller or sink |
| `src/controllers/AdminAuthFilter.h` | 106 | transport/controller or sink |
| `src/controllers/AiApiController.cc` | 519 | transport/controller or sink |
| `src/controllers/AiApiController.h` | 42 | transport/controller or sink |
| `src/controllers/ChannelController.cc` | 217 | transport/controller or sink |
| `src/controllers/ChannelController.h` | 32 | transport/controller or sink |
| `src/controllers/ControllerUtils.h` | 149 | transport/controller or sink |
| `src/controllers/HealthController.cc` | 80 | transport/controller or sink |
| `src/controllers/HealthController.h` | 31 | transport/controller or sink |
| `src/controllers/LogController.cc` | 130 | transport/controller or sink |
| `src/controllers/LogController.h` | 23 | transport/controller or sink |
| `src/controllers/MetricsController.cc` | 324 | transport/controller or sink |
| `src/controllers/MetricsController.h` | 38 | transport/controller or sink |
| `src/controllers/RateLimitFilter.h` | 77 | transport/controller or sink |
| `src/controllers/RetoolWorkspaceController.cc` | 379 | transport/controller or sink |
| `src/controllers/RetoolWorkspaceController.h` | 39 | transport/controller or sink |
| `src/controllers/sinks/ChatJsonSink.cpp` | 174 | transport/controller or sink |
| `src/controllers/sinks/ChatJsonSink.h` | 77 | transport/controller or sink |
| `src/controllers/sinks/ChatSseSink.cpp` | 292 | transport/controller or sink |
| `src/controllers/sinks/ChatSseSink.h` | 105 | transport/controller or sink |
| `src/controllers/sinks/ResponsesJsonSink.cpp` | 203 | transport/controller or sink |
| `src/controllers/sinks/ResponsesJsonSink.h` | 76 | transport/controller or sink |
| `src/controllers/sinks/ResponsesSseSink.cpp` | 446 | transport/controller or sink |
| `src/controllers/sinks/ResponsesSseSink.h` | 119 | transport/controller or sink |
| `src/dbManager/DbType.h` | 10 | infrastructure/store adapter |
| `src/dbManager/account/accountBackupDbManager.cpp` | 161 | infrastructure/store adapter |
| `src/dbManager/account/accountBackupDbManager.h` | 30 | infrastructure/store adapter |
| `src/dbManager/account/accountDbManager.cpp` | 591 | infrastructure/store adapter |
| `src/dbManager/account/accountDbManager.h` | 62 | infrastructure/store adapter |
| `src/dbManager/channel/channelDbManager.cpp` | 383 | infrastructure/store adapter |
| `src/dbManager/channel/channelDbManager.h` | 52 | infrastructure/store adapter |
| `src/dbManager/chaynsThread/chaynsThreadDbManager.cpp` | 382 | infrastructure/store adapter |
| `src/dbManager/chaynsThread/chaynsThreadDbManager.h` | 101 | infrastructure/store adapter |
| `src/dbManager/config/ConfigDbManager.cpp` | 141 | infrastructure/store adapter |
| `src/dbManager/config/ConfigDbManager.h` | 38 | infrastructure/store adapter |
| `src/dbManager/metrics/ErrorStatsDbManager.cpp` | 522 | infrastructure/store adapter |
| `src/dbManager/metrics/ErrorStatsDbManager.h` | 197 | infrastructure/store adapter |
| `src/dbManager/metrics/StatusDbManager.cpp` | 681 | infrastructure/store adapter |
| `src/dbManager/metrics/StatusDbManager.h` | 194 | infrastructure/store adapter |
| `src/dbManager/retoolWorkspace/RetoolWorkspaceDbManager.cpp` | 451 | infrastructure/store adapter |
| `src/dbManager/retoolWorkspace/RetoolWorkspaceDbManager.h` | 52 | infrastructure/store adapter |
| `src/dbManager/session/SessionDbManager.cpp` | 489 | infrastructure/store adapter |
| `src/dbManager/session/SessionDbManager.h` | 101 | infrastructure/store adapter |
| `src/domain/model/AccountData.h` | 133 | domain model/port (净化中) |
| `src/domain/model/BridgeWireFormat.h` | 21 | domain model/port (净化中) |
| `src/domain/model/ChannelInfo.h` | 99 | domain model/port (净化中) |
| `src/domain/model/ErrorEvent.h` | 206 | domain model/port (净化中) |
| `src/domain/model/ImageInfo.h` | 20 | domain model/port (净化中) |
| `src/domain/model/ProviderResult.h` | 152 | domain model/port (净化中) |
| `src/domain/model/RetoolWorkspaceInfo.h` | 144 | domain model/port (净化中) |
| `src/domain/model/SessionData.h` | 146 | domain model/port (净化中) |
| `src/domain/port/APIinterface.h` | 41 | domain model/port (净化中) |
| `src/domain/port/IAccountStore.h` | 44 | domain model/port (净化中) |
| `src/domain/port/IChannelStore.h` | 31 | domain model/port (净化中) |
| `src/domain/port/IKeyValueConfigStore.h` | 23 | domain model/port (净化中) |
| `src/domain/port/IRetoolWorkspaceStore.h` | 33 | domain model/port (净化中) |
| `src/main.cc` | 433 | runtime/composition root |
| `src/managedAccount/backends/ClassicProviderAccountBackend.cpp` | 101 | application/managed account |
| `src/managedAccount/backends/ClassicProviderAccountBackend.h` | 14 | application/managed account |
| `src/managedAccount/backends/IManagedAccountBackend.h` | 19 | application/managed account |
| `src/managedAccount/backends/RetoolWorkspaceBackend.cpp` | 61 | application/managed account |
| `src/managedAccount/backends/RetoolWorkspaceBackend.h` | 14 | application/managed account |
| `src/managedAccount/contracts/ManagedAccount.h` | 53 | application/managed account |
| `src/managedAccount/service/ManagedAccountService.cpp` | 60 | application/managed account |
| `src/managedAccount/service/ManagedAccountService.h` | 32 | application/managed account |
| `src/metrics/ErrorStatsConfig.cpp` | 110 | infrastructure/metrics |
| `src/metrics/ErrorStatsConfig.h` | 89 | infrastructure/metrics |
| `src/metrics/ErrorStatsService.cpp` | 350 | infrastructure/metrics |
| `src/metrics/ErrorStatsService.h` | 158 | infrastructure/metrics |
| `src/retoolWorkspace/RetoolWorkspaceManager.cpp` | 128 | application/workspace |
| `src/retoolWorkspace/RetoolWorkspaceManager.h` | 42 | application/workspace |
| `src/retoolWorkspace/RetoolWorkspaceService.cpp` | 180 | application/workspace |
| `src/retoolWorkspace/RetoolWorkspaceService.h` | 28 | application/workspace |
| `src/sessionManager/actionProtocol/ActionProtocolAdapter.cpp` | 75 | application/session pipeline |
| `src/sessionManager/actionProtocol/ActionProtocolAdapter.h` | 28 | application/session pipeline |
| `src/sessionManager/actionProtocol/ActionProtocolCompiler.cpp` | 451 | application/session pipeline |
| `src/sessionManager/actionProtocol/ActionProtocolCompiler.h` | 189 | application/session pipeline |
| `src/sessionManager/continuity/ContinuityResolver.cpp` | 144 | application/session pipeline |
| `src/sessionManager/continuity/ContinuityResolver.h` | 42 | application/session pipeline |
| `src/sessionManager/continuity/HistoryReplayBudget.cpp` | 310 | application/session pipeline |
| `src/sessionManager/continuity/HistoryReplayBudget.h` | 42 | application/session pipeline |
| `src/sessionManager/continuity/OutboundBudget.cpp` | 140 | application/session pipeline |
| `src/sessionManager/continuity/OutboundBudget.h` | 40 | application/session pipeline |
| `src/sessionManager/continuity/ResponseIndex.cpp` | 204 | application/session pipeline |
| `src/sessionManager/continuity/ResponseIndex.h` | 86 | application/session pipeline |
| `src/sessionManager/continuity/TextExtractor.cpp` | 21 | application/session pipeline |
| `src/sessionManager/continuity/TextExtractor.h` | 22 | application/session pipeline |
| `src/sessionManager/contracts/GenerationEvent.h` | 197 | application/session pipeline |
| `src/sessionManager/contracts/GenerationRequest.h` | 184 | application/session pipeline |
| `src/sessionManager/contracts/IResponseSink.h` | 130 | application/session pipeline |
| `src/sessionManager/core/ClientOutputSanitizer.cpp` | 86 | application/session pipeline |
| `src/sessionManager/core/ClientOutputSanitizer.h` | 69 | application/session pipeline |
| `src/sessionManager/core/Errors.h` | 186 | application/session pipeline |
| `src/sessionManager/core/GenerationService.cpp` | 532 | application/session pipeline |
| `src/sessionManager/core/GenerationService.h` | 219 | application/session pipeline |
| `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp` | 2214 | application/session pipeline |
| `src/sessionManager/core/RequestAdapters.cpp` | 1257 | application/session pipeline |
| `src/sessionManager/core/RequestAdapters.h` | 140 | application/session pipeline |
| `src/sessionManager/core/Session.cpp` | 1257 | application/session pipeline |
| `src/sessionManager/core/Session.h` | 342 | application/session pipeline |
| `src/sessionManager/core/SessionCodec.cpp` | 205 | application/session pipeline |
| `src/sessionManager/core/SessionCodec.h` | 26 | application/session pipeline |
| `src/sessionManager/core/SessionExecutionGate.h` | 295 | application/session pipeline |
| `src/sessionManager/tooling/BridgeHelpers.cpp` | 188 | application/session pipeline |
| `src/sessionManager/tooling/BridgeHelpers.h` | 68 | application/session pipeline |
| `src/sessionManager/tooling/BridgeProtocolCodec.cpp` | 513 | application/session pipeline |
| `src/sessionManager/tooling/BridgeProtocolCodec.h` | 83 | application/session pipeline |
| `src/sessionManager/tooling/ForcedToolCallGenerator.cpp` | 51 | application/session pipeline |
| `src/sessionManager/tooling/ForcedToolCallGenerator.h` | 29 | application/session pipeline |
| `src/sessionManager/tooling/StrictClientRules.cpp` | 200 | application/session pipeline |
| `src/sessionManager/tooling/StrictClientRules.h` | 76 | application/session pipeline |
| `src/sessionManager/tooling/ToolCallBridge.cpp` | 252 | application/session pipeline |
| `src/sessionManager/tooling/ToolCallBridge.h` | 256 | application/session pipeline |
| `src/sessionManager/tooling/ToolCallNormalizer.cpp` | 41 | application/session pipeline |
| `src/sessionManager/tooling/ToolCallNormalizer.h` | 30 | application/session pipeline |
| `src/sessionManager/tooling/ToolCallValidator.cpp` | 687 | application/session pipeline |
| `src/sessionManager/tooling/ToolCallValidator.h` | 227 | application/session pipeline |
| `src/sessionManager/tooling/ToolDefinitionEncoder.cpp` | 29 | application/session pipeline |
| `src/sessionManager/tooling/ToolDefinitionEncoder.h` | 22 | application/session pipeline |
| `src/sessionManager/tooling/ToolDefinitionResolver.h` | 176 | application/session pipeline |
| `src/sessionManager/tooling/XmlTagToolCallCodec.cpp` | 1314 | application/session pipeline |
| `src/sessionManager/tooling/XmlTagToolCallCodec.h` | 77 | application/session pipeline |
| `src/tools/ZeroWidthEncoder.cpp` | 204 | infrastructure/tool |
| `src/tools/ZeroWidthEncoder.h` | 123 | infrastructure/tool |
| `src/tools/accountlogin/login_client.cpp` | 85 | infrastructure/tool |
| `src/utils/BackgroundTaskQueue.h` | 162 | platform/infrastructure utility |
| `src/utils/ConfigValidator.cpp` | 183 | platform/infrastructure utility |
| `src/utils/ConfigValidator.h` | 20 | platform/infrastructure utility |
| `src/utils/IoLoopResponseStream.h` | 151 | platform/infrastructure utility |
| `src/utils/LoginResponseLogSummary.h` | 147 | platform/infrastructure utility |
| `src/utils/NexosRegistrationMailPolicy.h` | 186 | platform/infrastructure utility |
| `src/utils/NexosUserAgent.h` | 44 | platform/infrastructure utility |
| `src/utils/chaynsBrowserImpersonation.h` | 340 | platform/infrastructure utility |
