# aiapi

基于 Drogon 框架的 AI API 网关服务，提供 OpenAI 兼容的 Chat Completions 和 Responses API 接口。

> 最后更新：2026-08-15

## 目录

- [功能特性](#功能特性)
- [架构概览](#架构概览)
- [当前源码与 target](#当前源码与-target)
- [完整 API 端点清单](#完整-api-端点清单)
  - [AI 核心 API](#ai-核心-apiaiapicontroller)
  - [账号管理 API](#账号管理-apiaccountcontroller)
  - [Retool Workspace API](#retool-workspace-apiretoolworkspacecontroller)
  - [渠道管理 API](#渠道管理-apichannelcontroller)
  - [监控与日志 API](#监控与日志-apimetricscontroller--logcontroller)
  - [健康检查 API](#健康检查-apihealthcontroller)
- [核心模块说明](#核心模块说明)
  - [GenerationService](#generationservice生成编排服务)
  - [Tool Bridge 机制](#tool-bridge-机制tooling-模块)
  - [会话连续性](#会话连续性continuity-模块)
  - [客户端适配](#客户端适配)
  - [Codex XML 工具桥接](#codex-xml-工具桥接)
  - [会话追踪](#会话追踪)
  - [并发门控](#并发门控)
  - [错误统计系统](#错误统计系统)
- [API 使用示例](#api-使用示例)
- [构建、测试与运行](#构建测试与运行)
- [Provider 数据归档与恢复](#provider-数据归档与恢复)
- [配置说明](#配置说明)
- [错误码](#错误码)
- [详细文档](#详细文档)
- [单元测试](#单元测试)
- [开发路线](#开发路线)

## 功能特性

- ✅ OpenAI Chat Completions API 兼容（流式/非流式）
- ✅ OpenAI Responses API 兼容（流式/非流式，含 previous_response_id 续聊）
- ✅ 活跃 Provider：chaynsapi / retoolapi（ProviderBase + 注入式窄 Registry）
- ✅ Retool Workspace Provider（对外 HTTP：`/retoolapi/v1/*`）
- ✅ 退役兼容边界：历史 `/nexosapi/v1/*` 路由稳定返回 HTTP 410；具体 Nexos/OpenAiProvider 实现已删除
- ✅ 工具调用（Tool Calls）完整支持
- ✅ 工具调用桥接（XML Bridge）— 为不原生支持工具调用的通道提供桥接
- ✅ 工具调用验证（ToolCallValidator）— 支持 None/Relaxed/Strict 三种校验模式
- ✅ 参数形状规范化（ToolCallNormalizer）— 自动修复常见参数格式问题
- ✅ 工具定义编码（ToolDefinitionEncoder）— compact/full 两种模式
- ✅ 强制工具调用兜底（ForcedToolCallGenerator）— tool_choice=required 场景
- ✅ 严格客户端规则（StrictClientRules）— Kilo-Code / RooCode / Codex 客户端适配
- ✅ Codex XML 工具桥接 — 在上游不支持原生函数调用时，通过 XML 格式转发工具请求
- ✅ 外部状态需求识别 — 自动识别文件、仓库、命令、构建和测试等请求并要求执行工具
- ✅ 会话追踪（Hash / ZeroWidth 两种模式）
- ✅ 会话连续性决策（ContinuityResolver + TextExtractor）
- ✅ 历史回放预算（HistoryReplayBudget）— 按完整 turn 截取近期历史，超限整段省略并写入提示，不截断单条内容
- ✅ 响应索引（ResponseIndex）— Responses API GET/DELETE 支持
- ✅ 并发门控（SessionExecutionGate + CancellationToken + RAII Guard）
- ✅ 输出清洗（ClientOutputSanitizer）
- ✅ 统一错误模型（`platform::Error/ErrorCode`）+ 错误统计（ErrorStatsService + ErrorStatsConfig）
- ✅ 账号池管理（自动注册、Token 刷新、类型检测、轮转、备份）
- ✅ ManagedAccount 抽象层（传统账号 + Retool Workspace 统一管理入口）
- ✅ Retool Workspace 资产管理（workspace/session/workflow/agent 元数据持久化）
- ✅ Retool Workspace 创建入口（通过 aiapi_tool 内部编排执行）
- ✅ 渠道管理（多渠道、状态控制、并发限制）
- ✅ 服务状态监控（请求/错误时序、渠道与模型状态；JSON Metrics API）
- ✅ 内置日志查看 API（文件列表、尾部读取、过滤）
- ✅ 管理接口认证（AdminAuthFilter）
- ✅ 请求限流（RateLimitFilter）
- ✅ 配置校验（ConfigValidator）
- ✅ 后台任务队列（BackgroundTaskQueue）
- ✅ 健康检查端点（/health + /ready）
- ✅ 正式 production target 复用的 CTest 行为/集成测试套件
- ✅ Chat/Responses JSON 与 SSE Sink 分离，统一处理流式和非流式输出

## 架构概览

```
┌─────────────────────────────────────────────────────────────────┐
│                         HTTP 层                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │              Controllers + Filters                      │    │
│  │  AiApiController     — AI 核心 API 路由                 │    │
│  │  AccountController   — 账号管理 API                     │    │
│  │  RetoolWorkspaceController — Retool Workspace API       │    │
│  │  ChannelController   — 渠道管理 API                     │    │
│  │  MetricsController   — 监控指标 API                     │    │
│  │  LogController       — 日志查看 API                     │    │
│  │  HealthController    — 健康检查 API                     │    │
│  │  AdminAuthFilter     — 管理接口认证                     │    │
│  │  RateLimitFilter     — 请求限流                         │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        适配层                                    │
│  ┌─────────────────┐     ┌──────────────────────────────────┐  │
│  │ RequestAdapters │ ──▶ │      GenerationRequest           │  │
│  │ (Chat/Responses)│     │ (统一请求结构)                    │  │
│  └─────────────────┘     └──────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      生成编排层                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │ GenerationService（稳定 facade）→ GenerationPipeline     │    │
│  │  - materializeRequest() + ContinuityResolver              │    │
│  │  - ExecutionGuard + ProviderResponse / retry / commit    │    │
│  │  - GenerationResponsePipeline（结果处理 + 事件发送）      │    │
│  └─────────────────────────────────────────────────────────┘    │
│     │           │              │              │              │   │
│     ▼           ▼              ▼              ▼              ▼   │
│  ┌────────┐ ┌────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│  │ToolCall│ │Session │ │ToolCall  │ │Output    │ │Continuity│  │
│  │Bridge  │ │Manager │ │Validator │ │Sanitizer │ │Resolver  │  │
│  └────────┘ └────────┘ └──────────┘ └──────────┘ └──────────┘  │
│     │                                                           │
│     ▼                                                           │
│  ┌──────────────────┐                                           │
│  │SessionExecution  │ (并发门控 + CancellationToken)            │
│  │Gate              │                                           │
│  └──────────────────┘                                           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Provider 层                                 │
│  ┌─────────────┐     ┌─────────────────────────────────────┐   │
│  │ProviderReg- │ ──▶ │ IChatProvider / ProviderBase        │   │
│  │istry (注入) │     │ - generate(ProviderRequest, Context)│   │
│  │ 构造后冻结   │     │ - Result<ProviderResponse>          │   │
│  └─────────────┘     └─────────────────────────────────────┘   │
│        │                 ├─ IProviderModelCatalog              │
│        │                 └─ IProviderThreadContext              │
│        ├── chayns (infrastructure/provider/chayns)            │
│        └── retool (infrastructure/provider/retool)              │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                       输出层                                     │
│  ┌───────────────┐  ┌───────────────┐  ┌───────────────────┐   │
│  │ IResponseSink │  │GenerationEvent│  │   HTTP Response   │   │
│  │  (接口)       │◀─│  (事件模型)    │──▶│  (JSON/SSE)       │   │
│  └───────────────┘  └───────────────┘  └───────────────────┘   │
│        │                                                         │
│        ├── ChatJsonSink      (Chat 非流式 JSON)                  │
│        ├── ChatSseSink       (Chat 流式 SSE)                     │
│        ├── ResponsesJsonSink (Responses 非流式 JSON)              │
│        ├── ResponsesSseSink  (Responses 流式 SSE)                │
│        ├── CollectorSink     (事件收集，内部用)                   │
│        └── NullSink          (丢弃输出，测试用)                   │
└─────────────────────────────────────────────────────────────────┘
```

## 当前源码与 target

P8-W1 已完成正式 target DAG，P8-W2 已将该 DAG 收口为同一份物理目录事实。每个
production `.cpp/.cc` 由一个且仅一个正式 CMake target 拥有，测试只链接这些 target，
不会重新编译生产实现。`aiapi_legacy` 及其 source list 与所有历史源码顶层目录均已删除；
`check_physical_layout.py` 持续禁止旧目录或错误物理层 source 复活。

```text
aiapi_platform
└─ aiapi_domain                 -> aiapi_platform
   └─ aiapi_application         -> aiapi_domain, aiapi_platform
aiapi_infrastructure            -> aiapi_domain, aiapi_platform, Drogon/OpenSSL/PostgreSQL
aiapi_transport                 -> aiapi_application, aiapi_domain, aiapi_platform, Drogon
aiapi_runtime                   -> aiapi_application, aiapi_infrastructure, aiapi_transport
aiapi (main.cc)                 -> aiapi_runtime
```

`aiapi_transport` 仍以 whole-archive 链接到最终可执行文件，仅用于保留 Drogon 的静态 Controller
注册对象；这不是 application 到 transport/infrastructure 的反向依赖。

当前 canonical tree：

```text
src/
├── platform/                   Result、ErrorCode、deadline、日志与通用值设施
├── domain/                     纯值对象、规则和 ports；不含 JsonCpp/Drogon/DB 类型
├── application/
│   ├── account/                账号选择、注册、token、health 与 worker stage
│   ├── channel/                渠道 facade/use case
│   ├── generation/             action protocol、continuity、contracts、core、tooling
│   ├── health/                 readiness use case
│   ├── metrics/                metrics use case
│   └── workspace/              Workspace facade/use case
├── infrastructure/
│   ├── account/                Drogon account HTTP adapter 与 clocks
│   ├── codec/                  infrastructure JSON codec
│   ├── config/
│   ├── executor/
│   ├── managedAccount/         backend、contract 与 service
│   ├── metrics/                ErrorStats config/service 与 decoder
│   ├── persistence/            DB adapter（account/channel/config/session/metrics/workspace/thread）
│   ├── provider/{chayns,limits,retool}/
│   └── workspace/              RetoolWorkspaceService
├── transport/
│   └── controllers/
│       ├── codecs/             HTTP JSON codec
│       ├── sinks/              JSON/SSE sink 与 event-loop adapter
│       └── Controller、filter、tombstone
├── runtime/                    AppContext 与唯一组合根 AppWiring
├── test/                       CTest 注册的单元、fixture 与停机测试
├── CMakeLists.txt
└── main.cc
```

顶层只允许这些正式层、`CMakeLists.txt` 和 `main.cc`。`tools/arch/check_physical_layout.py`
将 CMake source list 与目录层逐项核对，并拒绝 `accountManager/`、`sessionManager/`、`dbManager/`、
`controllers/` 等历史顶层目录复活；其 selftest 在 CI 中与正向 gate 一起运行。

### HTTP / IO 边界（ADR-09）

- Controller 在 transport 层读取 Drogon request，再把复制的 `Json::Value` 和
  `aiapi::RequestHeaders` 交给 `RequestAdapters`/use case；application 不接收
  `drogon::HttpRequestPtr`。
- Account 生命周期使用框架无关的 `IAccountHttpTransport` DTO；
  `infrastructure/account/DrogonAccountHttpTransport` 是唯一 Drogon adapter。
- `AppWiring` 在组合根读取 `custom_config`，按值注入 Account workflow 和 AI use case；
  application worker 不重新查询 Drogon runtime config。
- `transport/controllers/sinks/IoLoopResponseStream.h` 是 worker 回到 Drogon event loop 的 adapter；
  sink/application 不直接操作 loop-affine response。

`tools/arch/check_http_io_boundary.py` 扫描 application include closure 与全部 domain 文件，
同时检查 CMake link、Account adapter 和 runtime-config 接线；`--selftest` 会以内存变异证明
Drogon include 会被拒绝。

## 完整 API 端点清单

### AI 核心 API（AiApiController）

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/chaynsapi/v1/chat/completions` | Chat Completions（流式/非流式） |
| POST | `/chaynsapi/v1/responses` | Responses API（流式/非流式） |
| GET | `/chaynsapi/v1/responses/{id}` | 获取已创建的响应 |
| DELETE | `/chaynsapi/v1/responses/{id}` | 删除已创建的响应 |
| GET | `/chaynsapi/v1/models` | 获取可用模型列表 |
| POST | `/nexosapi/v1/chat/completions` | 已退役；稳定返回 HTTP 410 `provider_retired` |
| POST | `/nexosapi/v1/responses` | 已退役；稳定返回 HTTP 410 `provider_retired` |
| GET | `/nexosapi/v1/responses/{id}` | 已退役；稳定返回 HTTP 410 `provider_retired` |
| DELETE | `/nexosapi/v1/responses/{id}` | 已退役；稳定返回 HTTP 410 `provider_retired` |
| GET | `/nexosapi/v1/models` | 已退役；稳定返回 HTTP 410 `provider_retired` |
| GET | `/nexosapi/v1/account/quota` | 已退役；稳定返回 HTTP 410 `provider_retired` |
| POST | `/retoolapi/v1/chat/completions` | Retool Workspace Chat Completions |
| POST | `/retoolapi/v1/responses` | Retool Workspace Responses |
| GET | `/retoolapi/v1/responses/{id}` | 获取已创建的 Retool 响应 |
| DELETE | `/retoolapi/v1/responses/{id}` | 删除已创建的 Retool 响应 |
| GET | `/retoolapi/v1/models` | 获取 Retool 可用模型列表 |

### Nexos 退役兼容说明

- Nexos 和具体 `OpenAiProvider` 已退役，不能再配置、创建渠道或新增账号。
- 六条历史 `/nexosapi/v1/*` 路由保留 tombstone，统一返回 HTTP 410、`provider_retired` 和 `X-AIAPI-Retirement-Id`。
- 公开的 OpenAI Chat Completions/Responses **协议兼容能力仍保留**，由 `chaynsapi` 与 `retoolapi` 路由提供；这与删除具体 OpenAI 上游 Provider 是两个不同概念。
- 数据归档/恢复和发布步骤见 `doc/adr/work-products/P02-provider-data-retirement.md`。

### Retool Provider 说明

- `retoolapi` 通过 Retool Workspace 池路由请求；标准 OpenAI 兼容接口本身**不要求**显式传 `workspaceId`，未传时会从可用 workspace 池自动分配。
- 成功响应会在 `_meta` 中返回本次实际命中的 `workspaceId / routeType / provider / resourceName`，便于排查路由结果。
- `claude-*` 的 **workflow** 路径已支持，包括 `claude-sonnet-4-6`。
- `agent-claude-sonnet-4-6` **当前明确不支持**：Retool 原生 agent thread 链路会返回 Anthropic 上游错误  
  `This model does not support assistant message prefill. The conversation must end with a user message.`
- 因此当前支持性应按实际链路理解，而**不要仅以 `/retoolapi/v1/models` 暴露的模型名判断可用性**。

#### Retool Anthropic 兼容性说明（当前已验证）

| 模型 | workflow | agent |
|------|----------|-------|
| `claude-sonnet-4-20250514` | 支持 | `agent-claude-sonnet-4-20250514` 支持 |
| `claude-sonnet-4-5-20250929` | 支持 | `agent-claude-sonnet-4-5-20250929` 支持 |
| `claude-sonnet-4-6` | 支持 | `agent-claude-sonnet-4-6` **不支持** |

> 说明：`claude-opus-*` 是否可用还会受到目标 workspace 的 Anthropic 资源限流配置影响；若 workspace 侧 RPM 为 0，则会直接返回 rate limit 错误。

### 账号管理 API（AccountController）

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/aichat/account/add` | 批量添加账号（支持对象/数组） |
| POST | `/aichat/account/delete` | 批量删除账号（含上游删除） |
| POST | `/aichat/account/update` | 批量更新账号信息 |
| POST | `/aichat/account/refresh` | 异步刷新所有账号 token + 类型 |
| POST | `/aichat/account/autoregister` | 自动注册新账号（最多 20 个/次） |
| GET | `/aichat/account/info` | 获取内存中的账号列表 |
| GET | `/aichat/account/backupinfo` | 获取账号备份信息 |
| GET | `/aichat/account/dbinfo` | 获取数据库中的账号列表 |
| GET | `/aichat/account/settings` | 获取账号自动化设置 |
| POST | `/aichat/account/settings` | 更新账号自动化设置 |

### Retool Workspace API（RetoolWorkspaceController）

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/aichat/retool/workspace/create` | 调 aiapi_tool 完整创建 Retool workspace 并入库 |
| POST | `/aichat/retool/workspace/upsert` | 手动写入/覆盖 workspace 资产 |
| GET | `/aichat/retool/workspace/info` | 获取单个 workspace 信息 |
| GET | `/aichat/retool/workspace/list` | 获取 workspace 列表 |
| GET | `/aichat/retool/workspace/pool-status` | 获取 workspace 池状态 |
| POST | `/aichat/retool/workspace/disable` | 禁用 workspace |
| POST | `/aichat/retool/workspace/enable` | 启用 workspace |
| POST | `/aichat/retool/workspace/delete` | 删除 workspace |
| POST | `/aichat/retool/workspace/verify` | 本地验证 workspace 资产字段完整性 |

`create` 当前会同步调用 aiapi_tool 内部接口：

- `POST /api/v1/workflows/retool-workspace/provision-sync`

### 渠道管理 API（ChannelController）

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/aichat/channel/add` | 批量添加渠道 |
| POST | `/aichat/channel/delete` | 批量删除渠道 |
| POST | `/aichat/channel/update` | 更新渠道配置 |
| POST | `/aichat/channel/update-status` | 更新渠道启用/禁用状态 |
| GET | `/aichat/channel/list` | 获取渠道列表 |

### 监控与日志 API（MetricsController + LogController）

| 方法 | 路径 | 功能 |
|------|------|------|
| GET | `/aichat/metrics/requests/series` | 请求量时序统计 |
| GET | `/aichat/metrics/errors/series` | 错误量时序统计（多维过滤） |
| GET | `/aichat/metrics/errors/events` | 错误事件列表（分页） |
| GET | `/aichat/metrics/errors/events/{id}` | 错误事件详情 |
| GET | `/aichat/metrics/status/summary` | 服务状态概览 |
| GET | `/aichat/metrics/status/channels` | 渠道状态列表 |
| GET | `/aichat/metrics/status/models` | 模型状态列表 |
| GET | `/aichat/logs/list` | 日志文件列表 |
| GET | `/aichat/logs/tail` | 日志尾部读取（支持级别/关键词过滤） |

### 健康检查 API（HealthController）

| 方法 | 路径 | 功能 |
|------|------|------|
| GET | `/health` | 返回服务状态、版本、运行时长 |
| GET | `/ready` | 检查数据库、Provider、账号池可用性（依赖不足时返回 503） |

## 核心模块说明

### GenerationService（生成编排服务）

`GenerationService` 是保持 controller/use case 调用面不变的薄 facade；实际流程按阶段拆分：
- `GenerationPipeline.cpp` — 请求物化、连续性恢复、执行门控、工具 bridge 请求转换、Provider 调用/重试和会话提交；
- `GenerationResponsePipeline.cpp` — 输出清洗、原生/bridge 工具解析、identity/参数规范化、schema 校验、客户端规则、零宽会话 ID 与事件发射；
- `tooling/ForcedToolCallGenerator.cpp`、`ToolCallNormalizer.cpp`、`ToolDefinitionEncoder.cpp` — 可独立测试的工具规则。

```
runGuarded(req, sink, policy)
  └─ GenerationPipeline::run()
       ├─ materializeRequest() → session_st
       ├─ ContinuityResolver::resolve() + getOrCreateSession()
       ├─ ExecutionGuard(RAII) + request-scoped cancellation/deadline
       ├─ transformRequestForToolBridge()（按需）→ IChatProvider::generate()
       ├─ GenerationResponsePipeline::emit()
       │    ├─ sanitize + native/bridge decode
       │    ├─ forced tool + identity + 参数规范化 + schema 校验
       │    ├─ StrictClientRules + zero-width session ID
       │    └─ ToolCallDone → OutputTextDone → Completed
       └─ coverSessionresponse() → 会话上下文更新 + 转移
```

### Account workflows（账号工作流）

`AccountManager` 保持既有管理与 `IAccountSelector` port，但不再把所有流程放在一个实现文件：

```text
AccountManager（注入、配置、启动）
  ├─ AccountSelector + AccountSelectionPolicy
  │    └─ pool rebuild / rotation / free-pro eligibility / excluded users
  ├─ AccountRegistrationStateMachine
  │    └─ waiting → registering → active
  │       failure: waiting → delete
  ├─ AccountRegistrationWorkflow
  │    └─ Chayns register-and-login poll / Retool provision dispatch
  ├─ AccountTokenWorkflow
  │    └─ token validation / refresh / queued retry / reachable probe
  ├─ AccountHealthWorkflow
  │    └─ quota replenishment / expiry cleanup / upstream deletion
  └─ AccountWorkers
       └─ interruptible waits + deadline-aware join
```

HTTP transport 与 clock 是 infrastructure adapter；`AppWiring` 在 `AccountManager::init()` 前显式注入它们。
因此遗漏接线会安全降级而非由 application 层反向链接 infrastructure，且由启动接线门禁阻止。

### GenerationEvent（统一事件模型）

| 事件类型 | 说明 | 关键数据 |
|----------|------|----------|
| `Started` | 生成开始 | responseId, model |
| `OutputTextDelta` | 文本增量（流式） | delta, index |
| `OutputTextDone` | 文本完成 | text, index |
| `ToolCallDone` | 工具调用完成 | id, name, arguments, index |
| `Usage` | Token 使用量 | inputTokens, outputTokens |
| `Completed` | 生成完成 | finishReason (stop/tool_calls) |
| `Error` | 错误 | code, message, detail |

### Tool Bridge 机制（tooling/ 模块）

为不支持原生 Tool Calls 的上游通道提供 XML 桥接，模块已拆分为独立组件：

| 组件 | 文件 | 职责 |
|------|------|------|
| ToolCallBridge | `ToolCallBridge.h/cpp` | 桥接主逻辑（请求注入 + 响应解析） |
| ToolDefinitionEncoder | `ToolDefinitionEncoder.h/cpp` | 工具定义编码（compact/full 模式） |
| XmlTagToolCallCodec | `XmlTagToolCallCodec.h/cpp` | XML 格式工具调用编解码 |
| ToolCallValidator | `ToolCallValidator.h/cpp` | Schema 校验（None/Relaxed/Strict） |
| ToolCallNormalizer | `ToolCallNormalizer.h/cpp` | 参数形状规范化（数组/别名/默认值） |
| ForcedToolCallGenerator | `ForcedToolCallGenerator.h/cpp` | tool_choice=required 兜底生成 |
| StrictClientRules | `StrictClientRules.h/cpp` | Kilo-Code/RooCode 严格模式适配 |
| BridgeHelpers | `BridgeHelpers.h/cpp` | 桥接辅助函数 |

**请求侧**：
1. ToolDefinitionEncoder 将工具定义编码为文本格式
2. 生成随机触发标记（如 `<Function_Ab1c_Start/>`）
3. 构建 `<tool_instructions>` 提示注入到 request message

**响应侧**：
1. 通过触发标记定位 XML 块（防止误解析历史消息）
2. XmlTagToolCallCodec 解析 `<function_calls>/<function_call>` 结构
3. ToolCallNormalizer 参数规范化 + ToolCallValidator Schema 校验 + 降级策略

### 会话连续性（continuity/ 模块）

| 组件 | 职责 |
|------|------|
| ContinuityResolver | 决策当前请求是否属于已有会话的延续 |
| HistoryReplayBudget | 控制上游历史回放体积：按完整 conversation turn 保留最近消息；超单条/总预算时整段省略并插入提示，不截断原文 |
| ResponseIndex | 响应存储索引，支持 Responses API 的 GET/DELETE 操作 |
| TextExtractor | 从复杂消息结构中提取纯文本内容 |

`HistoryReplayBudget` 已接入当前活跃的 `chaynsapi` / `retoolapi` Provider 历史组装路径。可通过 `custom_config.history_replay` 调整预算（单位：字节）：

| 配置项 | 默认 | 说明 |
|--------|------|------|
| `max_request_bytes` | `262144`（256KiB） | 整次历史回放总预算 |
| `max_message_bytes` | `131072`（128KiB） | 单条消息上限；超出则整条替换为提示 |
| `max_tool_message_bytes` | `49152`（48KiB） | tool 角色消息上限（与单条上限取更小值） |

上限硬封顶为 8MiB。未配置时使用上表默认值。

### 客户端适配

| 客户端 | 标识 | 特殊处理 |
|--------|------|----------|
| Kilo-Code | `Kilo-Code` | 严格模式：`StrictClientRules` 注入 apply_diff SEARCH/REPLACE 精确匹配与失败恢复策略；配合 ToolCallValidator 做工具参数约束 |
| RooCode | `RooCode` | 同 Kilo-Code（仅 Roo/Kilo 启用严格客户端规则） |
| Claude Code | `claudecode` | 零宽会话 ID 在 tool_calls 前单独发送 |
| 其他 | — | 宽松模式，不强制 Roo/Kilo 专用规则 |

### Codex XML 工具桥接

当客户端或上游通道不支持原生 Provider/Recipient/Namespace/JSON 函数调用时，网关可以使用 XML Bridge 传递工具请求。桥接策略包含：

- 使用 `<function_calls>` / `<function_call>` XML 结构描述工具调用；
- 保留工具名称与 JSON 参数；
- 对 `tool_choice=required` 或当前请求明确依赖外部状态的场景强制要求执行工具；
- 识别文件、目录、仓库、Git、命令、构建、测试等关键词，避免在未检查外部状态时凭空回答；
- 收到带有 `[tool_result ...]` 的结果后继续生成最终响应；
- 支持并行工具调用配置。

该逻辑由 `GenerationPipeline.cpp`（请求 bridge / Provider 调用）、
`GenerationResponsePipeline.cpp`（响应 bridge / 事件）和 `tooling/` 的专职规则组件共同承担；
请求适配位于 `RequestAdapters.cpp`。

## 会话追踪

| 模式 | 实现 | 说明 |
|------|------|------|
| Hash | 消息内容 SHA256 | 默认模式，基于 systemPrompt + messages 哈希 |
| ZeroWidth | 零宽字符嵌入 | 在助手回复中嵌入不可见的 sessionId |

### 并发门控

- **RejectConcurrent**：同一会话有请求在执行时，新请求返回 409 Conflict
- **CancelPrevious**：取消之前的请求，执行新请求
- 使用 RAII `ExecutionGuard` 自动管理生命周期

### 错误统计系统

错误按 4 个域分类：

| 域 | 说明 | 典型事件 |
|------|------|----------|
| `SESSION_GATE` | 会话并发门控 | 并发冲突、请求取消 |
| `UPSTREAM` | 上游 Provider | HTTP 错误、超时 |
| `TOOL_BRIDGE` | 工具桥接 | XML 未找到、校验过滤、降级、强制生成 |
| `INTERNAL` | 内部异常 | 运行时异常、未知错误 |

配置通过 `ErrorStatsConfig` 管理，支持运行时调整保留策略。

### HTTP 过滤器

| 过滤器 | 作用范围 | 说明 |
|--------|----------|------|
| AdminAuthFilter | `/aichat/*` | Bearer Token 认证，`admin_api_key` 为空时跳过（向后兼容） |
| RateLimitFilter | AI API 端点 | 令牌桶限流，可配置 `requests_per_second` 和 `burst` |

## API 使用示例

### Chat Completions API

```bash
# 非流式
curl -X POST "http://localhost:55555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "messages": [{"role": "user", "content": "Hello"}]
  }'

# 流式
curl -N -X POST "http://localhost:55555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "stream": true,
    "messages": [{"role": "user", "content": "Hello"}]
  }'
```

### Responses API

```bash
# 创建 Response
curl -X POST "http://localhost:55555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "input": "Hello"
  }'

# 续聊
curl -X POST "http://localhost:55555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "previous_response_id": "resp_abc123",
    "input": "Tell me more."
  }'

# 获取 Response
curl "http://localhost:55555/chaynsapi/v1/responses/{response_id}"

# 删除 Response
curl -X DELETE "http://localhost:55555/chaynsapi/v1/responses/{response_id}"
```

### 账号管理

```bash
# 添加账号
curl -X POST "http://localhost:55555/aichat/account/add" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY" \
  -d '{
    "apiname": "chaynsapi",
    "username": "user@example.com",
    "password": "xxx"
  }'

# 自动注册 5 个账号
curl -X POST "http://localhost:55555/aichat/account/autoregister" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY" \
  -d '{"apiname": "chaynsapi", "count": 5}'

# 刷新所有账号状态
curl -X POST "http://localhost:55555/aichat/account/refresh" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY"
```

### 渠道管理

```bash
# 添加渠道
curl -X POST "http://localhost:55555/aichat/channel/add" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY" \
  -d '[{
    "channelname": "main",
    "channeltype": "chaynsapi",
    "channelurl": "https://api.example.com",
    "channelkey": "sk-xxx",
    "maxconcurrent": 10,
    "supports_tool_calls": false
  }]'

# 获取渠道列表
curl "http://localhost:55555/aichat/channel/list" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY"
```

### 监控

```bash
# 服务状态概览
curl "http://localhost:55555/aichat/metrics/status/summary" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY"

# 错误时序统计（最近 24 小时）
curl "http://localhost:55555/aichat/metrics/errors/series" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY"

# 日志尾部（过滤 ERROR 级别）
curl "http://localhost:55555/aichat/logs/tail?lines=100&level=ERROR" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY"

# 健康检查
curl "http://localhost:55555/health"
curl "http://localhost:55555/ready"

# 监控指标示例
curl "http://localhost:55555/metrics"
```

## 构建、测试与运行

### 依赖

- C++17 编译器、CMake、Drogon、JsonCpp、OpenSSL、spdlog；
- 持久化按配置使用 PostgreSQL / MySQL / SQLite3；
- 运行 Provider 退役脚本还需要 `sqlite3` CLI。

### 从仓库根目录构建

```bash
cd aiapi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
```

### 测试与架构门禁

```bash
# CTest 与直接 runner（runner 用于排查单个 Drogon case）
ctest --test-dir build --output-on-failure
build/src/test/aiapi_test

# 已实施的结构门禁
python3 tools/architecture_audit.py --selftest
python3 tools/architecture_audit.py --baseline doc/adr/audits/audit-baseline.json
python3 tools/arch/check_source_ownership.py --require-no-legacy
python3 tools/arch/check_target_layers.py --require-no-legacy
python3 tools/arch/check_http_io_boundary.py --selftest
```

覆盖、Address/Undefined sanitizer 与 ThreadSanitizer 的完整发布命令见
[`doc/adr/work-products/P08-transition-cleanup.md`](doc/adr/work-products/P08-transition-cleanup.md)。
TSan 必须从干净的 `build-tsan/` 开始：`rm -rf build-tsan && tools/run-tsan.sh`。

### 运行

```bash
build/src/aiapi
```

服务默认监听 `0.0.0.0:55555`。

### Docker 构建与运行

```bash
# 方式一：环境变量注入配置
docker compose -f docker-compose.env.yml up --build aiapi

# 方式二：卷挂载配置文件
cp config.example.json config.json
# 编辑 config.json 填入实际配置
docker compose -f docker-compose.volume.yml up --build aiapi

# 方式三：SQLite + 卷挂载持久化
cp config.sqlite.example.json config.json
mkdir -p data logs cores
# config.json 中默认使用 ./data/aiapi.db，对应宿主机 ./data/aiapi.db
docker compose -f docker-compose.volume.yml up --build aiapi
```

`docker-compose.env.yml` 与 `docker-compose.volume.yml` 中的服务名、镜像名、容器名均统一为 `aiapi`。

Docker 入口脚本支持：
- `CONFIG_JSON` 环境变量 → 直接覆盖配置文件
- `CUSTOM_CONFIG` 环境变量 → 使用 jq 合并到现有配置

## Provider 数据归档与恢复

退役的是 `nexosapi` / `openai` 的**具体上游 Provider 数据**，不是 OpenAI 兼容 HTTP 协议，也不包括
Retool 的合法 `openai_resource_*` 字段。迁移脚本只针对当前 SQLite schema：

```text
tools/migrations/provider_retirement_preflight_v1.sql  # 只读预检
tools/migrations/retire_providers_v1.sql               # 事务归档并删除目标 live rows
tools/migrations/restore_retired_providers_v1.sql      # 冲突即停止的恢复
tools/migrations/test_provider_retirement.py           # 离线往返/失败路径演练
```

先阅读 [`tools/migrations/README.md`](tools/migrations/README.md)，再在维护窗口按“备份 → 只读
preflight → 副本往返演练 → 原库 retire”的顺序执行。archive snapshot 含账号凭据和 session payload，
不得写入日志或提交到仓库。代码回滚不会自动恢复数据；需要恢复时只能使用同一
`retirement_id` 的 restore 脚本，主键或 `channelname` 冲突会整批终止而不会覆盖 live 数据。

## 配置说明

配置文件位于 `config.example.json`，主要配置项：

```json
{
  "listeners": [
    { "address": "0.0.0.0", "port": 55555 }
  ],
  "db_clients": [
    { "name": "aichatpg", "rdbms": "postgresql", "host": "...", "...": "..." }
  ],
  "app": {
    "number_of_threads": 4,
    "log": {
      "use_spdlog": true,
      "log_level": "DEBUG"
    },
    "cors": { "enabled": true, "allow_origins": ["*"] }
  },
  "plugins": [
    { "name": "drogon::plugin::PromExporter", "config": { "path": "/metrics" } },
    { "name": "drogon::plugin::AccessLogger" }
  ],
  "custom_config": {
    "dbtype": "sqlite3",
    "admin_api_key": "",
    "session_tracking": {
      "mode": "zerowidth"
    },
    "tool_bridge": {
      "definition_mode": "compact",
      "include_descriptions": false,
      "max_description_chars": 5000,
      "trigger_random_length": 8,
      "strict_sentinel": true
    },
    "login_service_urls": [
      { "name": "chaynsapi", "url": "http://login-service:8004/api/v1/logins" }
    ],
    "regist_service_urls": [
      { "name": "chaynsapi", "url": "http://orchestrator-service:8000/api/v1/workflows/register-and-login" }
    ]
  }
}
```

### 账号自动化策略

账号自动化策略默认从 `custom_config.account_automation` 提供初始值；运行时优先从数据库配置表 `app_config` 读取，若表中缺失配置项则自动写入默认值。

- `auto_delete_enabled`：是否自动删除过期的 free 账号
- `delete_after_days`：账号创建超过多少天后删除，默认 `6`
- `auto_register_enabled`：当渠道账号数量不足时，是否自动补注册账号

### 关键配置项

| 配置路径 | 说明 | 可选值 |
|----------|------|--------|
| `custom_config.dbtype` | 数据库类型 | `postgresql` / `mysql` / `sqlite3` |
| `custom_config.admin_api_key` | 管理接口 Bearer Key（为空则兼容放行并告警） | 任意非空字符串 |
| `custom_config.session_tracking.mode` | 会话追踪模式 | `hash` / `zerowidth` |
| `custom_config.tool_bridge.definition_mode` | 工具定义编码模式 | `compact` / `full` |
| `custom_config.tool_bridge.include_descriptions` | 是否包含工具描述 | `true` / `false` |
| `custom_config.tool_bridge.max_description_chars` | 描述截断长度 | 0-5000 |
| `custom_config.tool_bridge.trigger_random_length` | 触发标记随机长度 | 6-12 |
| `custom_config.tool_bridge.strict_sentinel` | 严格哨兵模式（全局默认） | `true` / `false` |
| `custom_config.tool_bridge.strict_sentinel_by_channel` | 按渠道覆盖严格哨兵 | `{ "channel": bool }` |
| `custom_config.tool_bridge.strict_sentinel_by_model` | 按模型覆盖严格哨兵 | `{ "model": bool }` |
| `custom_config.tool_bridge.rewrite_user_input_conflicts` | 是否改写用户输入中的冲突指令 | `true` / `false` |
| `custom_config.rate_limit.enabled` | AI 接口限流开关 | `true` / `false` |
| `custom_config.rate_limit.requests_per_second` | 每秒令牌补充速率 | 正整数 |
| `custom_config.rate_limit.burst` | 瞬时突发上限 | 正整数 |
| `custom_config.session_persistence.memory_expire_hours` | 内存会话 TTL（小时，可为小数） | 正数，默认 `24` |
| `custom_config.session_persistence.memory_cleanup_interval_hours` | 过期会话轮询清理间隔（小时，可为小数） | 正数且不大于 TTL，默认 `1` |
| `custom_config.session_persistence.db_retention_hours` | 数据库会话快照保留期（小时，可为小数） | 正数，建议 ≥ TTL，默认 `24` |
| `custom_config.session_persistence.store_session_payload` | 是否将会话 payload 写入 `chat_session_state` | `true` / `false` |
| `custom_config.session_persistence.store_response_body` | 是否将响应体随 `response_index` 落库 | `true` / `false` |
| `custom_config.response_index.max_entries` | Responses 索引最大内存条目数 | 正整数 |
| `custom_config.response_index.max_age_hours` | Responses 索引过期时间（小时） | 正整数 |
| `custom_config.response_index.cleanup_interval_minutes` | 索引清理周期（分钟） | 正整数 |
| `custom_config.upstream_error_texts` | 上游错误文本匹配列表 | 字符串数组 |
| `custom_config.cors.allowed_origins` | CORS 白名单 | 字符串数组 |

- `history_replay`：历史回放预算（`max_request_bytes` / `max_message_bytes` / `max_tool_message_bytes`，默认 256KiB / 128KiB / 48KiB）

#### 会话持久化（`custom_config.session_persistence`）

三个时间参数**统一以小时为单位**，支持小数（`0.5` = 30 分钟、`0.25` = 15 分钟）。程序启动时读取并换算为秒（四舍五入，最小 1 秒）后应用到会话清理线程，日志会打印实际生效值：

```
[会话持久化] 参数生效: 内存TTL=24h, 内存清理间隔=1h, DB保留=24h, payload落库=on, response_body落库=off
```

```json
"session_persistence": {
  "memory_expire_hours": 24,
  "memory_cleanup_interval_hours": 1,
  "db_retention_hours": 24,
  "store_session_payload": true,
  "store_response_body": false
}
```

| 参数 | 作用 |
|------|------|
| `memory_expire_hours` | `session_map` 中会话的空闲存活时长；超时后被清理线程淘汰，并同步删除对应 DB 行（避免懒加载“复活”过期会话） |
| `memory_cleanup_interval_hours` | 清理线程轮询周期，决定过期判定的时间精度。**必须不大于** `memory_expire_hours`，否则启动校验失败 |
| `db_retention_hours` | `chat_session_state` 快照保留期，防止表无限增长。若小于 `memory_expire_hours` 会输出告警——活跃会话的快照将被提前清理，进程重启后无法懒加载恢复 |
| `store_session_payload` | 关闭后不再写入会话快照，`response_index` 映射不受影响 |
| `store_response_body` | 关闭后仅落库响应索引映射而不存响应正文，可显著降低存储占用 |

> **迁移提示**：旧版的 `memory_expire_seconds` / `memory_cleanup_interval_seconds` / `db_retention_seconds`（单位：秒）已废弃且不再生效。为避免老配置直接启动失败，配置校验器对这些键仅输出告警而非报错，请尽快改用对应的 `*_hours` 键。

## 错误码

| 错误码 | HTTP Status | 说明 |
|--------|-------------|------|
| BadRequest | 400 | 请求格式错误 |
| Unauthorized | 401 | 未授权 |
| Forbidden | 403 | 禁止访问 |
| NotFound | 404 | 资源不存在 |
| Conflict | 409 | 并发冲突（同一会话已有请求在执行） |
| RateLimited | 429 | 限流 |
| Timeout | 504 | 超时 |
| ProviderError | 502 | Provider 错误 |
| Internal | 500 | 内部错误 |
| Cancelled | 499 | 请求被取消 |

## 详细文档

### 设计与架构

| 文档 | 说明 |
|------|------|
| [调用关系图与接口样例](doc/aiapi_callflow_and_api_examples.md) | 详细的模块拆解、时序图、数据结构和 curl 示例 |
| [开发计划](doc/development-plan.md) | 项目整体开发计划与里程碑 |
| [优化报告](doc/optimization-report.md) | 性能优化分析与改进记录 |
| [错误统计开发计划](doc/error_stats_dev_plan.md) | 错误统计系统开发计划 |
| [错误统计与监控方案](doc/aiapi%20错误统计与监控方案（设计文档）) | 错误统计与监控系统设计文档 |
| [服务状态监控设计](doc/service_status_monitoring_design.md) | 服务状态监控系统设计 |

### 会话连续性模块

| 文档 | 说明 |
|------|------|
| [会话模块索引](doc/session/README.md) | 会话连续性模块文档入口 |
| [重构设计](doc/session/session_continuity_refactor_design.md) | 会话连续性重构设计方案 |
| [重构开发计划](doc/session/session_continuity_refactor_development_plan.md) | 会话连续性重构开发计划 |

### 源码内文档

| 文档 | 说明 |
|------|------|
| [sessionManager 模块](src/application/generation/README.md) | 会话管理核心模块说明 |
| [contracts 层](src/application/generation/contracts/README.md) | 接口契约说明 |
| [core 层](src/application/generation/core/README.md) | 核心服务说明 |
| [continuity 模块](src/application/generation/continuity/README.md) | 会话连续性模块说明 |
| [tooling 模块](src/application/generation/tooling/README.md) | 工具调用模块说明 |

## 单元测试

测试列表的唯一真值是 [`src/test/CMakeLists.txt`](src/test/CMakeLists.txt) 的 `TEST_SOURCES`。
它链接 `aiapi_runtime` 和正式 production target，`check_test_registration.py --require-strict`
在已配置 build 中核对源文件、Drogon case 和 CTest 注册是否一致。

覆盖范围包括：RequestAdapters/JSON contract、generation pipeline 与 tool bridge、Chayns/Retool
离线 fixture、Account lifecycle/worker、统一 `Result/Error` 与 ProviderBase NVI、Controller injection、
队列背压、AppContext shutdown 与五类 SIGTERM 场景。P8 收口时 Debug `ctest` 为 **396/396**，
直接 runner 为 **396 cases / 2075 assertions**；运行时覆盖快照见
[`P08-coverage.md`](doc/adr/work-products/P08-coverage.md)。请使用上面的 root-level
`ctest --test-dir build` 命令，而不要从 `src/test` 单独配置一个会复制 production source 的测试工程。

## 开发路线

- [x] Chat Completions API 基础功能
- [x] Responses API 基础功能（含 previous_response_id 续聊）
- [x] 流式输出支持（CollectorSink → SSE 分块传输）
- [x] 工具调用支持（原生 + Bridge）
- [x] 工具调用桥接（XML Bridge + 随机 Sentinel）
- [x] 工具调用验证（None/Relaxed/Strict + 降级策略）
- [x] 参数形状规范化（ToolCallNormalizer：数组/别名/默认值）
- [x] 工具定义编码（ToolDefinitionEncoder：compact/full）
- [x] 强制工具调用兜底（ForcedToolCallGenerator）
- [x] 严格客户端规则（StrictClientRules：Kilo-Code / RooCode）
- [x] 会话追踪（Hash/ZeroWidth + ContinuityResolver + TextExtractor）
- [x] 并发门控（SessionExecutionGate + CancellationToken + RAII Guard）
- [x] 输出清洗（ClientOutputSanitizer）
- [x] 统一错误模型（`platform::Error/ErrorCode`）
- [x] 错误统计系统（ErrorStatsService + ErrorStatsConfig + 4 域分类）
- [x] 账号池管理（自动注册 + Token 刷新 + 类型检测 + 备份）
- [x] 渠道管理（CRUD + 状态控制 + supports_tool_calls）
- [x] 服务状态监控（Summary + Channels + Models）
- [x] 日志查看 API（文件列表 + 尾部读取 + 过滤）
- [x] 服务状态/错误统计 Metrics API（JSON；非 Prometheus exposition 格式）
- [x] 增量流式响应（AsyncStreamResponse + SSE 实时推送）
- [x] 活跃 Provider（chaynsapi + retoolapi）与 OpenAI 兼容公开协议
- [x] Nexos/OpenAiProvider 可恢复退役与 410 tombstone
- [x] HTTP 过滤器（AdminAuthFilter + RateLimitFilter）
- [x] 健康检查端点（/health + /ready）
- [x] 配置校验（ConfigValidator）
- [x] 后台任务队列（BackgroundTaskQueue）
- [x] 控制器拆分（6 个独立控制器）
- [x] sessionManager 分层重构（contracts / core / continuity / tooling）
- [x] HistoryReplayBudget（多 Provider 历史回放预算与完整 turn 截取）
- [x] P8 架构收口（六个正式 target、无 legacy owner、ADR-09 IO boundary、物理目录 gate）
- [x] 正式 target 复用的 CTest 行为、fixture、sanitizer 与 SIGTERM 测试

## License

MIT


最后更新时间：2026-08-15
