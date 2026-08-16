# Provider 分层实施计划

> 本计划遵循 ADR-01、ADR-07、ADR-09 以及 `doc/multi-protocol-generation/`。
> Provider 的具体实现继续位于 `infrastructure`；application 只依赖 domain/provider ports。

## 目标结构

```text
application
  GenerationPipeline / ProviderInvocationStage
        |
domain/port
  IChatProvider / IProviderModelCatalog / IProviderThreadContext
        |
infrastructure/provider
  ProviderBase
    Provider Orchestrator
      Protocol Client
      Conversation/Thread Context
      Retry/Polling Policy
      HTTP Transport
```

## 阶段

### P0：边界冻结（已完成）

- application 不 include 具体 Provider、Drogon 或 HTTP client；
- Provider 通过 `ProviderBase` 和窄能力 port 暴露；
- transport 只负责 HTTP/JSON/SSE/IO loop；
- Provider 的 upstream ID、账号和 workspace 状态不泄漏到 application。

### P1：跨 Provider 契约与观测（已完成）

- 保持 `IChatProvider`、`IProviderModelCatalog`、`IProviderThreadContext` 为稳定端口；
- 为 Provider 内部组件统一 `ProviderRequest`、`ProviderResponse`、`Deadline`、
  `CancellationToken` 和 `platform::Error` 边界；
- 所有跨层日志携带 `requestId`、`conversationId`、`provider`，有 upstream thread 时再携带
  `upstreamThreadId`；
- application、provider、transport 日志不得重复实现业务决策。

### P2：Chayns 垂直切片（已完成）

将当前 `chaynsapi` 中的职责逐步拆为：

- `ChaynsProvider`：账号选择、流程编排和重试决策；
- `ChaynsProtocolClient`：thread/message/image wire 请求和响应解析；
- `ChaynsThreadContext`：本地 conversation 到 upstream thread 的映射；
- `ChaynsPollingLoop`：可取消轮询、退避、消息关联；
- `ChaynsHttpTransport`：纯 HTTP/连接/timeout。

每次拆分保持现有 ProviderBase、thread ledger、Pro/free 路由和请求语义不变，并先以 fixture
锁定行为，再移动实现。

当前进度：策略边界已抽取到 `ChaynsProviderPolicy`；图片、thread/message、personId、模型目录、
上游线程删除的 wire 请求和响应解析已抽取到 `ChaynsProtocolClient`。本地 conversation 映射、
台账恢复/迁移/分离已由 `ChaynsThreadContext` 持有，可取消轮询、退避和消息关联已由
`ChaynsPollingLoop` 持有。账号选择、历史裁剪及重试编排由 `ChaynsProvider` orchestrator 负责；
旧的 `chaynsapi` 类型名仅作为兼容别名保留，composition root 已使用显式的
`ChaynsProvider`。现有离线 Provider fixture 覆盖 free/pro、续聊、重启恢复、超时和取消，组件
fixture 另行覆盖 thread context 的持久化生命周期。

### P3：Retool 垂直切片（已完成）

- `RetoolProvider`：workspace 选择和 workflow/agent 分支编排；
- `RetoolWorkflowClient`：workflow wire 协议；
- `RetoolAgentClient`：agent/thread wire 协议；
- `RetoolWorkspaceContext`：workspace/thread affinity 和 usage；
- `RetoolHttpTransport`：纯 HTTP/连接/timeout。

workflow 和 agent 的差异不通过 Chayns 抽象强行统一。

当前进度：`RetoolWorkspaceContext` 已接管 workspace affinity、agent thread 映射、transfer/erase
以及 usage lease；`RetoolWorkflowClient` 和 `RetoolAgentClient` 已分别接管 workflow、agent/thread
的请求构造、认证头和 timeout 边界，共用的 Retool cookie/header envelope 位于
`RetoolProtocolHttp`。实际 orchestrator 已命名为 `RetoolProvider`，`retoolapi` 仅保留兼容别名；
composition root 显式构造 `RetoolProvider`。配置解析后的 history/bootstrap 限制通过
`RetoolProviderSettings` 注入，不再由 provider 读取 Drogon singleton。`retoolapi` 仍负责 workspace
选择、模板编排、响应语义映射和轮询决策，两条协议的请求/轮询语义保持独立。

### P4：错误、取消和日志收口

- Provider 协议错误在 infrastructure adapter 映射为 `platform::Error`；
- 所有阻塞点使用同一个绝对 deadline 和只读 cancellation token；
- 分层日志使用相同 correlation 字段，并对应用/Provider/transport 文件实施一致轮转策略；
- 禁止在 Provider 中写 session aggregate 或直接访问项目 singleton。

当前进度：Retool HTTP 状态到 `platform::Error` 的映射已收口到
`RetoolProtocolHttp`，workflow/agent client 通过 `ResponseResult` 把 transport、取消和 deadline
失败作为 typed `platform::Error` 返回；Chayns/Retool 的 provider 配置和 browser profile 均从
composition root 注入。Retool 与 Chayns 轮询等待新增 cancellation-aware clock seam，production
clock 以短片段等待并在同一绝对 deadline 上复查，fixture clock 保持确定性。Retool orchestrator
的请求、workspace、run/thread 日志现已统一携带 `requestId`、`conversationId` 和上游 ID；JSON
响应解码现在经过 `decodeJsonBody`，畸形 payload 映射为带 `invalid_json` provider code 的 typed
错误。Chayns 消息提交结果也携带 transport/HTTP/decode `platform::Error`，并由 orchestrator
保留最后一次 typed submission failure。Retool slice gate 现会逐条检查 `[retoolapi]` 诊断日志的
request/conversation correlation 字段。

### P5：门禁和验证

- 增加 Provider 内部层依赖和 source ownership 检查；
- 扩展 Chayns/Retool Provider contract fixture；
- 验证 Chat、Responses、工具桥接、续聊、超时、取消、重试和线程回收；
- 运行完整 build、CTest、架构门禁和 sanitizer。

当前进度：Chayns ledger port 已从 persistence 目录提升到 `domain/port/IChaynsThreadLedger.h`，
provider context 不再直接 include 具体 persistence 层，并同步收紧 DB include ratchet。Chayns/Retool
slice gate 已更新为检查显式 `*Provider` orchestrator、兼容别名、内部 client/context/transport
组件、禁止 direct HTTP/singleton 回退以及 composition-root production wiring；两道 gate 的 mutation
selftest 均通过，并固定 polling/run 日志的 provider/request/conversation/upstream correlation 字段。
严格测试注册为 437/437，production source ownership 为 106/106，普通构建与完整 CTest
均为 437/437 通过；此前 ASan+UBSan 构建与 CTest 已为 436/436 通过，无 sanitizer/runtime
diagnostics。新增 204-empty-poll 行为测试确认 Chayns 轮询空批次不再产生 WARN。
（`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`、
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`）。非 Provider 的既有
architecture-audit 棘轮仍保持为外部待办：R2 基线超出、`src/main.cc` 的既有 layer rule
以及旧 HTTP/IO boundary 规则要求的 `RequestAdapters.*` 缺口，均不属于本切片且未被回滚。

## 实施规则

1. 先抽取纯策略和协议边界，再移动有状态流程；
2. 每一步保持可编译、可回滚，不建立第二套 Provider registry；
3. 不为简单转发创建空壳 stage/class；
4. Chayns 和 Retool 只共享稳定端口、错误/取消契约以及真正通用的策略。
