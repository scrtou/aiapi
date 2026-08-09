# P1-W3 · Retool workflow/agent characterization

| 项 | 值 |
|---|---|
| 状态 | DONE |
| 前置 | P1-W2 Chayns 离线假上游已完成 |
| 生产实现 | `src/apipoint/retoolapi/retoolapi.cpp` |
| 输入来源 | 合成 characterization；本地不存在 Retool HAR |
| 网络约束 | 全部测试使用内存 fake，未创建 socket，未访问真实上游 |

## 1. 目标与结论

本工作项锁定了 `retoolapi::requestWorkflow` 与 `retoolapi::requestAgent` 的当前生产行为：

- 两条路径均由真实 `retoolapi.cpp` 执行，不以 helper 或 Provider stub 替代；
- request method/path、认证头存在性、关键 body 字段及 provider binding 被离线断言；
- 显式 workspace、conversation affinity 和 usage acquire/release 被真实 Manager + 内存 Store 验证；
- workflow/agent 成功、401、429、5xx、无效 JSON、FAILED 和轮询超时有行为证据；
- 轮询的 120/180 秒墙钟等待由 fake clock 瞬时推进；
- coverage 已证明 `requestWorkflow` 和 `requestAgent` 被执行。

未找到 Retool 录制输入，因此 fixture 不冒充 HAR-derived。仓库内现有 5 个 ignored HAR 均属于 Chayns/Genspark；本工作项只提交明确标记为 `synthetic-characterization` 的合成数据。

## 2. 当前调用图

### 2.1 共同入口、workspace 与 usage

```text
retoolapi::generate(session)
  ├─ ChannelManager::getChannelList()
  │    └─ retoolapi 显式禁用 → AuthError
  ├─ model 以 "agent-" 开头 → requestAgent(session)
  └─ 其他 model             → requestWorkflow(session)

requestWorkflow / requestAgent
  ├─ resolveWorkspaceId(session, requireAgent)
  │    ├─ clientInfo.workspace_id/workspaceId → 写入 conversationWorkspaceMap_
  │    ├─ conversationWorkspaceMap_ 命中      → 回填 clientInfo.workspace_id
  │    └─ RetoolWorkspaceManager::listWorkspaces()
  │         ├─ 过滤 disabled、非 passed/ready、空 baseUrl、缺 route ID
  │         └─ 按 inUseCount、lastUsedAt、createdAt 排序后固定亲和
  ├─ ScopedWorkspaceUsage(workspaceId)
  │    ├─ 构造：markWorkspaceUsageStarted
  │    └─ 析构：仅 acquire 成功时 markWorkspaceUsageFinished
  └─ ManagedAccountService::buildExecutionContext(RetoolWorkspace, workspaceId)
       └─ RetoolWorkspaceBackend → RetoolWorkspaceManager → IRetoolWorkspaceStore
```

当前 RAII 的精确语义是：workspace 找到后、execution context 读取前 acquire；所有正常/错误 return 均经析构 release。若 acquire 自身失败，则 guard 不调用 release。

### 2.2 Workflow 请求序列

```text
GET  {baseUrl}/api/workflow/{workflowId}                 timeout 30s
  ├─ 读取 workflow.templateData
  └─ 无匹配 resource 时：
       GET {baseUrl}/api/resources                       timeout 30s
       └─ 回写 openAI/Anthropic resource 到 workspace store

Anthropic 且显式配置 RETOOL2_* clone source 时：
GET  {RETOOL2_BASE_URL}/api/workflow/{sourceWorkflowId} timeout 30s
  └─ clone source，保留 destination identity，再 patch
否则：直接 patch destination workflow

POST {baseUrl}/api/workflow/{workflowId}                 timeout 60s
  body = patched workflow
POST {baseUrl}/api/workflow/run                          timeout 30s
  body.workflowId = workflowId
GET  {baseUrl}/api/workflowRun/getBlockLevelLogs?runId={runId}
  ├─ 最多 120 次，每个未完成状态 sleep 1s
  ├─ code1.status=SUCCESS → output.data → trim → ProviderResult.text
  └─ code1.status=FAILED  → output.error → InternalError
```

OpenAI binding：

```text
providerId           = retoolAIBuiltIn::openAI
providerName         = openAI
providerResourceName = workspace.openaiResourceName
subtype              = OpenAIProviderQuery
```

Anthropic binding：

```text
providerId           = retoolAIBuiltIn::anthropic
providerName         = anthropic
providerResourceName = workspace.anthropicResourceName
subtype              = AnthropicQuery
```

成功 meta：`workspaceId/workspace_id`、`routeType=workflow`、`resourceId`、`model`、`provider`、`providerId`、`resourceName`。

### 2.3 Agent 请求序列

```text
GET  {baseUrl}/api/workflow/{agentId}                    timeout 30s
[可选] GET {baseUrl}/api/resources                       timeout 30s
POST {baseUrl}/api/workflow/{agentId}                    timeout 60s
  body = patched agent workflow；model 去掉 "agent-" 前缀
POST {baseUrl}/api/agents/{agentId}/threads              timeout 30s
  body = {name: "aiapi-thread", timezone: "UTC"}
POST {baseUrl}/api/agents/{agentId}/threads/{threadId}/messages
  body = {type: "text", text: lastUserContent, timezone: "UTC"}
GET  {baseUrl}/api/agents/{agentId}/logs/{runId}
     ?startAfterUUID=00000000-0000-7000-8000-000000000000&limit=100
  ├─ 最多 180 次，每个未完成状态 sleep 1s
  ├─ COMPLETED → trace[-1].data.data.content → trim → text
  └─ FAILED    → trace[-1].data.error → InternalError
```

`agentThreadMap_` 按 conversation 复用 thread。缓存 thread 返回 404 且 body 含 `Thread not found` 时，当前实现会删除缓存、创建替代 thread、按预算 replay system/user/assistant 历史、等待每个 replay run，再重发当前消息。该增强分支本工作项已登记，但没有录制输入，仍是后续补强项。

成功 meta 与 workflow 相同，但 `routeType=agent`、`resourceId=agentId`，`model` 为去掉 `agent-` 前缀后的实际模型。

## 3. 错误映射真值

| 触发点 | 当前 ProviderError | statusCode | 证据 |
|---|---|---:|---|
| channel disabled / workspace 不可解析 / context 不存在 | `AuthError` | 401 | 源码审计；workspace 成功路径已执行 |
| HTTP 401/403 | `AuthError` | 原 HTTP | 401 fixture 已执行 |
| HTTP 404（普通分类） | `InvalidRequest` | 404 | 源码审计；agent 特定 404 另走 recreate |
| HTTP 408/504 | `Timeout` | 504 | `classifyHttpError` 当前会丢失 408，统一为 504 |
| HTTP 429 | `RateLimited` | 429 | fixture 已执行 |
| HTTP 5xx | `InternalError` | 原 HTTP | 503 fixture 已执行 |
| transport 非 Ok/空 response | 多数调用点为 `NetworkError`；save 为合成 503 | 500/503 | transport 只返回空指针，当前丢失具体 ReqResult |
| workflow HTTP 200 + invalid JSON | `Unknown` | **200** | invalid JSON fixture 已执行 |
| workflow/agent poll FAILED | `InternalError` | 500 | agent FAILED fixture 已执行 |
| workflow 120 次未完成 | `Timeout` | 504 | fake clock 120 次 sleep |
| agent 180 次未完成 | `Timeout` | 504 | fake clock 180 次 sleep |
| agent message 缺 run ID | `InternalError` | 500 | 源码审计 |

`HTTP 200 + invalid JSON → error=Unknown,statusCode=200` 是已锁定的历史缺陷，不是推荐契约；P6 的统一 `Result/Error` 工作必须在迁移测试中显式决定是否修正，不能静默改变。

## 4. 离线 fixture 与安全边界

提交 4 个语义 fixture：

```text
src/test/fixtures/retool/
├── workflow-success.json
├── agent-success.json
├── http-errors.json
└── invalid-json.json
```

共同规则：

- `source` 必须为 `synthetic-characterization`；
- URL 只使用 RFC 2606 保留域名 `*.invalid`；
- 不保存 token、cookie、authorization、password、API key 或 email；
- 认证只断言 `x-xsrf-token`/`cookie` 头“存在”，不保存值；
- workspace/thread/run/resource ID、正文和错误均为合成值；
- `tools/fixtures/check_retool_fixtures.py` 检查固定文件集合、来源标记、JWT/email、secret-bearing 字段和非保留 host。

验证：

```text
python3 tools/fixtures/check_retool_fixtures.py
Retool fixture safety: 4 synthetic files PASS
```

## 5. 窄测试缝

新增：

```text
IRetoolHttpTransport::send(baseUrl, request, timeoutSeconds)
IRetoolClock::now()
IRetoolClock::sleepFor(milliseconds)
```

生产默认分别使用 Drogon `HttpClient::sendRequest` 和真实 steady clock/sleep；测试 fake 只脚本化 response 和推进时间。以下策略仍留在 `retoolapi`，没有被错误下沉到 transport/clock：

- workspace 选择与亲和；
- usage 生命周期；
- provider/resource binding；
- workflow/agent patch；
- retry/poll/replay 次数；
- path/body 和错误映射。

同时删除了 Anthropic clone source 的源码内 live URL/JWT/XSRF/workflow/resource 默认值。clone 现在必须由 `RETOOL2_*` 显式完整配置；未配置时沿用既有 fallback，直接 patch destination workflow。该修复避免默认生产路径携带源码凭据或意外访问固定 workspace。

## 6. 测试矩阵

真实 `retoolapi.cpp` 的 8 个离线测试：

| 测试 | 锁定行为 |
|---|---|
| `RetoolProvider_WorkflowFixtureRunsRealRequestWorkflowOffline` | workflow method/path/body、OpenAI binding、RUNNING→SUCCESS、meta、usage |
| `RetoolProvider_AgentFixtureRunsRealRequestAgentOffline` | agent method/path/body、Anthropic binding、thread/message、RUNNING→COMPLETED、meta、usage |
| `RetoolProvider_WorkspaceAffinityPersistsAfterExplicitSelection` | 显式 workspace 后同 conversation 无显式 ID 仍命中 affinity |
| `RetoolProvider_WorkflowErrorMappingsComeFromProductionPath` | 401/429/503 分类及错误路径 release |
| `RetoolProvider_InvalidJsonMappingIsCharacterized` | 200 invalid JSON 的历史 Unknown/200 行为 |
| `RetoolProvider_WorkflowPollingTimeoutUsesFakeClock` | 120 次轮询，无墙钟等待，Timeout |
| `RetoolProvider_AgentFailedRunIsMappedToInternalError` | FAILED trace error 映射 |
| `RetoolProvider_AgentPollingTimeoutUsesFakeClock` | 180 次轮询，无墙钟等待，Timeout |

验证结果：

```text
normal ctest:   244/244 PASS
coverage ctest: 244/244 PASS
Retool targeted: 8/8 PASS
```

受控突变证据（不提交突变）：将生产实现 workflow 成功 meta 的
`routeType="workflow"` 临时改为 `"workflow-mutated"`，仅运行
`RetoolProvider_WorkflowFixtureRunsRealRequestWorkflowOffline`，测试以
`mutation_exit=8` 失败（`"workflow-mutated" == "workflow"`）；随后恢复源码并重建通过。
这证明断言绑定了生产实现语义，而非只验证 fake transport。

## 7. Coverage 证据

来源：`doc/adr/work-products/P01-runtime-coverage-report.md`。

```text
production implementation files = 69
instrumented implementation files = 51
not instrumented = 18

retoolapi.cpp
  lines    = 406/889 (45.67%)
  branches = 677/3140 (21.56%)

retoolapi::requestWorkflow
  execution_count = 8
  lines            = 60/75 (80.00%)
  branches         = 125/344 (36.34%)

retoolapi::requestAgent
  execution_count = 3
  lines            = 114/315 (36.19%)
  branches         = 205/1288 (15.92%)
```

`RetoolHttpTransport.cpp`/`RetoolClock.cpp` 在 test target 中被 instrument 但未执行是预期：离线测试必须使用 fake，不能以执行 production socket/real sleep 来追求覆盖。

## 8. 遗留问题与后续边界

- cached thread 404 recreate/history replay 分支尚无权威录制输入；P6 Retool vertical slice 前应补合成状态机测试或获得脱敏录制；
- transport 当前把所有非 Ok `ReqResult` 折叠成空 response，无法区分 DNS/connect/timeout；统一错误契约阶段需修正；
- invalid JSON 的 Unknown/200 是明确迁移测试输入；
- `ScopedWorkspaceUsage`、ManagedAccountService、ChannelManager 仍是单例访问，按 P5/P6 计划后续注入；
- workflow/agent 仍在每次请求覆盖保存 workflow，属于业务语义而非 transport 职责；本阶段不擅自重写；
- 本工作项不证明 HTTP Controller、event-loop、断连或 SIGTERM，相关边界仍属于 P1-W5。

## 9. 回滚

- 删除 `RetoolHttpTransport.*`、`RetoolClock.*` 和注入构造器，并把 `sendJsonRequest`/sleep 恢复为 Drogon/标准库直接调用，可独立回滚测试缝；
- 删除 `src/test/fixtures/retool/`、fixture checker、Provider 测试和 test target source，可独立回滚安全网；
- 不应恢复已删除的源码内 live Retool 凭据；如必须使用 Anthropic clone source，只能通过运行时 secret 配置 `RETOOL2_*`。
