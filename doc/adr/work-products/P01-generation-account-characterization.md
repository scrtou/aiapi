# P1-W4 · Generation/Account 权威实现 characterization

| 项 | 值 |
|---|---|
| 状态 | DONE |
| 前置 | P1-W1 运行时覆盖、P1-W2 Chayns、P1-W3 Retool 已完成 |
| 生产实现 | `GenerationService::{runGuarded,emitResultEvents}`、`toolcall::{transformRequestForToolBridge,generateForcedToolCall,normalizeToolCallArguments}`、`AccountManager` |
| 网络约束 | 全部新增测试使用内存 Provider/store/http fake 与 fake clock；未访问真实上游、未真实 sleep |
| 核心结果 | Generation tooling 由生产 pipeline 与单测共用唯一实现；架构审计 R1 从 4 降为 0 |

## 1. 目标、输入与结论

本工作项针对阶段 1 审计映射中的两个高风险入口建立生产路径安全网：

1. 从 `GenerationService::runGuarded` 进入真实 session、ToolBridge、Provider 和 emit pipeline，锁定请求转换、codec 解码、forced tool、参数归一和事件相对顺序；
2. 从 `AccountManager` 真实入口锁定内存增删改、token 失效、账号池重建、自动注册成功激活与失败回滚；
3. 消除 `GenerationService` 私有成员与 `sessionManager/tooling` 自由函数的 4 个同名竞争项，避免单测通过 helper 而生产调用另一份实现；
4. 通过 coverage 和受控突变证明测试确实执行并约束生产代码。

输入来自当前源码行为和合成响应，不使用录制的账号/Generation 流量。新增 fake 数据只包含
`*.invalid` 邮箱或 synthetic ID/token，不含真实凭据。

结论：P1-W4 的权威入口、离线性、运行时执行和突变门禁均已满足。P1 阶段仍未结束；SIGTERM、队列、断连 harness 与 ASan 基础运行属于 P1-W5。

## 2. Generation 权威实现与调用图

### 2.1 请求与 Provider 调用

```text
GenerationService::runGuarded(request, sink)
  └─ buildSession(request)
      └─ executeGuardedWithSession(session, sink)
          ├─ resolve continuity / session gate
          ├─ ChannelManager 查询 supportsToolCalls
          ├─ 不支持原生 tools：
          │    └─ toolcall::transformRequestForToolBridge(session)
          │         ├─ 解析 client capabilities 与 bridge wire format
          │         ├─ 编码 tool definitions / action contract
          │         ├─ 改写 message/system context
          │         ├─ 保留 toolsRaw
          │         └─ 清空 provider 不可消费的 structured tools
          ├─ executeProvider(session)
          │    └─ ApiManager → APIinterface::generate(session)
          └─ emitResultEvents(session, sink)
```

`test_generation_service_bridge_fixture.cpp` 注入内存 `CapturingProvider`，但不替代
`GenerationService`。测试从 `runGuarded` 进入真实 production pipeline，并在 Provider 边界观察改写后的 `session_st`。

### 2.2 响应转换与 emit 顺序

```text
GenerationService::emitResultEvents(session, sink)
  ├─ sink.onEvent(Started)
  ├─ ProviderResult → text / native tool calls
  ├─ bridge channel：BridgeProtocolCodec::decode
  ├─ 无调用且 tool_choice=required：
  │    └─ toolcall::generateForcedToolCall
  ├─ annotateToolCallIdentities
  ├─ 显式 forced tool 名过滤
  ├─ Codex/Roo completion 适配
  ├─ toolcall::normalizeToolCallArguments
  │    ├─ 空参数 → {}
  │    ├─ malformed JSON → {}
  │    ├─ 非 object JSON → {"value": ...}
  │    └─ 已知 schema 的 alias/array/mode 精细归一
  ├─ ToolCallValidator 校验/过滤/降级
  ├─ toolcall::applyStrictClientRules
  ├─ ToolCallDone（若有）
  ├─ OutputTextDone（可包含连续性模式的零宽 session ID）
  ├─ Completed(finishReason)
  └─ sink.onClose()
```

测试锁定的是事件的协议相对顺序，而不是错误地假定永远只有三个事件：`Started` 必须在首位、
`ToolCallDone` 必须早于可选 `OutputTextDone`、`Completed` 必须在末位。零宽连续性事件是当前合法行为，不能为了固定数量而删除。

forced fallback 的权威契约为：只有 `tool_choice=required` 或显式 forced function 且存在工具定义时才生成调用；没有定义时不得凭空发明 `attempt_completion`。生成参数以 schema 必填字段和当前输入为依据。

## 3. Generation R1 权威实现收口

修改前审计报告的 4 个 R1 同名竞争项：

```text
applyStrictClientRules
generateForcedToolCall
normalizeToolCallArguments
transformRequestForToolBridge
```

收口方式：

- `GenerationService.cpp` 直接调用 `toolcall::transformRequestForToolBridge`；
- emit pipeline 直接调用 `toolcall::{generateForcedToolCall,normalizeToolCallArguments,applyStrictClientRules}`；
- 删除 `GenerationService.h` 中对应私有成员声明和无意义的 forwarding member；
- 将原先位于 `GenerationServiceEmitAndToolBridge.cpp` 的完整生产逻辑定义为 tooling namespace 的权威自由函数；
- 删除三份简化竞争实现 `ForcedToolCallGenerator.cpp`、`ToolCallNormalizer.cpp`、`ToolDefinitionEncoder.cpp`，保留其窄公共头作为函数端口；
- production/test CMake 均只链接完整实现，不再分别选择不同 implementation。

门禁结果：

```text
architecture_audit R1: 4 → 0
architecture ratchet: PASS
```

这一步不是提前执行 P7 pipeline 重写；它只消除会让 P1 characterization 测错对象的竞争实现。

## 4. Account 权威实现与调用图

### 4.1 内存账号增删改与选择

```text
AccountManager::addAccountbyPost(row)
  └─ 检查重复 → addAccount(...) → accountList / accountPool

AccountManager::updateAccount(row)
  └─ 替换 accountList 条目 → rebuildPoolLocked(api)

AccountManager::deleteAccountbyPost(api, user)
  └─ 从 accountList 删除 → rebuildPoolLocked(api)

AccountManager::getAccountByUserName(api, user, out)
  └─ 当前历史行为：命中时 useCount += 1
```

characterization 明确记录两个当前缺陷/迁移输入：

- `addAccountbyPost`、`updateAccount`、`deleteAccountbyPost` 只改内存结构，不写 `IAccountStore`；
- `getAccountByUserName` 不是纯读，命中时会增加 `useCount`。

P7/P8 重写账号 workflow 时必须显式决定新契约，并用迁移测试说明行为变化，不能静默把当前行为当成理想设计。

### 4.2 token 失效与账号池

```text
AccountManager::checkToken()
  └─ 遍历内存账号
      └─ checkChaynsToken(token)
          └─ IAccountHttpTransport::send(GET token endpoint)
      └─ 401/false → setStatusTokenStatus(..., false)
          └─ rebuildPoolLocked(api)

getEligibleAccount(api, AnyValid)
  └─ 从重建后的 heap/pool 中排除 tokenStatus=false 账号
```

### 4.3 自动注册成功与回滚

```text
AccountManager::autoRegisterAccount(api)
  ├─ ChannelManager 检查渠道状态
  ├─ IAccountStore::createWaitingAccount(api) → waitingId
  ├─ update status: waiting → registering
  ├─ POST /api/v1/workflows/register-and-login
  │    └─ IAccountHttpTransport::send
  ├─ GET /api/v1/workflows/{taskId}（轮询）
  │    ├─ 未完成/暂时失败 → IAccountClock::sleepFor(3s)
  │    └─ succeeded → 解析账号/session/site 结果
  ├─ 成功：IAccountStore::activateAccount(waitingId, account)
  │    └─ addAccount(...) 加载内存索引/池
  └─ 任一失败：rollbackWaitingAccount(waitingId)
       ├─ update status: registering → waiting
       └─ deleteWaitingAccount(waitingId)
```

测试覆盖 HTTP 503 立即失败和一次 create + 一次 succeeded poll。fake clock 证明这些分支不产生墙钟等待；需要重试/超时的 300 次轮询分支已由注入缝变得可测，但本工作项未穷举。

## 5. Account 窄测试缝

新增：

```text
IAccountHttpTransport::send(baseUrl, request, timeoutSeconds)
IAccountClock::sleepFor(milliseconds)
```

生产默认实现分别封装 Drogon `HttpClient` 和 `std::this_thread::sleep_for`。策略仍保留在 `AccountManager`：

- endpoint/path/method/body/header；
- token 判定与池失效；
- workflow 状态机与轮询预算；
- waiting/registering/active 状态转换；
- activate/rollback 事务顺序。

源码检查表明 `src/accountManager` 中直接网络和 sleep 只存在于默认 adapter：

```text
AccountHttpTransport.cpp: HttpClient::newHttpClient
AccountClock.cpp: std::this_thread::sleep_for
```

测试 transport 只消费内存 response 队列并记录 method/path/header；测试 clock 只记录 duration。新增测试不创建 socket、不访问真实上游、不等待真实时间。

## 6. 测试矩阵

### 6.1 Generation 生产 pipeline

| 测试 | 锁定行为 |
|---|---|
| `GenerationService_ToolBridgeTransformsRequestThroughRunGuarded` | `runGuarded` 进入真实 transform；Provider 看到 bridge instructions、`toolsRaw` 和 wire format |
| `GenerationService_BridgeCodecAndEmitOrderRunThroughProductionPipeline` | 真实 codec 解码 `read_file`；Started/ToolCall/可选零宽文本/Completed 相对顺序 |
| `GenerationService_NativeToolArgumentsAreNormalizedBeforeEmit` | native 空 arguments 在真实 emit 中归一为 `{}` |
| `GenerationService_RequiredToolFallbackRunsInsideEmitResultEvents` | 显式 forced function 在真实 emit 中产生 `ping` 调用 |
| `ForcedToolCall_*`（5 项） | 既有调用保持、无定义不发明、required 选择、call ID、JSON 参数 |
| `ToolCallNormalizer_*`（5 项） | empty/malformed/non-object/object/multiple 的公共 event-boundary 契约 |

### 6.2 Account 生命周期

| 测试 | 锁定行为 |
|---|---|
| `AccountLifecycle_InMemoryAddUpdateDeleteCharacterization` | 内存 add/update/delete、重复处理、lookup 增加 useCount、store 未写入 |
| `AccountLifecycle_CheckTokenUsesFakeHttpAndInvalidatesPool` | fake GET/Authorization、401 失效、eligible pool 排除 |
| `AccountLifecycle_AutoRegisterHttpFailureRollsBackWaitingRow` | registering → waiting → deleteWaiting，guard 清理 |
| `AccountLifecycle_AutoRegisterSuccessActivatesAndLoadsAccount` | create → poll succeeded → activate → 内存可查，path 和账号类型 |

验证：

```text
Generation/forced/normalizer/account targeted: 18/18 PASS
normal ctest:                             252/252 PASS
coverage ctest:                           252/252 PASS
strict test registration:                 252/252 exact
```

## 7. Coverage 证据

来源：`doc/adr/work-products/P01-runtime-coverage-report.md`。

```text
GenerationServiceEmitAndToolBridge.cpp
  lines    = 478/1102 (43.38%)
  branches = 743/3241 (22.93%)

toolcall::transformRequestForToolBridge  exec=3   lines=127/188
toolcall::generateForcedToolCall         exec=7   lines=78/140
toolcall::normalizeToolCallArguments     exec=13  lines=48/161
GenerationService::emitResultEvents      exec=8   lines=134/245
GenerationService::runGuarded            exec=8   lines=20/20
GenerationService::executeProvider       exec=8   lines=22/30

accountManager.cpp
  lines    = 524/1765 (29.69%)
  branches = 983/6941 (14.16%)

AccountManager::addAccountbyPost         exec=2  lines=16/17
AccountManager::updateAccount            exec=2  lines=18/19
AccountManager::deleteAccountbyPost      exec=2  lines=10/10
AccountManager::checkToken               exec=1  lines=18/23
AccountManager::autoRegisterAccount      exec=2  lines=133/236
AccountManager::rollbackWaitingAccount   exec=1  lines=4/4
AccountManager::rebuildPoolLocked        exec=11 lines=11/12
AccountManager::loadAccount              exec=18 lines=12/13
```

coverage target 已同步从已删除的
`GenerationService::transformRequestForToolBridge` 改为权威
`toolcall::transformRequestForToolBridge`，并增加 forced/normalize 与 Account 生命周期入口，防止报告继续搜索不存在的旧符号。

## 8. 受控突变证据

未提交的受控突变：将生产函数 `rollbackWaitingAccount` 的状态恢复从
`AccountStatus::WAITING` 临时改为 `AccountStatus::ACTIVE`，重建后只运行：

```text
AccountLifecycle_AutoRegisterHttpFailureRollsBackWaitingRow
```

结果：`mutation_exit=8`，测试在 `test_account_lifecycle_fixture.cpp:252` 失败；恢复源码、重新构建后同一测试 1/1 PASS。这证明测试断言绑定生产回滚顺序，不是只验证 fake 自身。

## 9. 门禁结果

```text
architecture_audit --selftest: PASS
architecture ratchet:          PASS (R1=0, no regression)
cycle/layer gate:              PASS (0 cycle, no new violation)
startup wiring:                PASS
test registration --strict:    PASS (252 declarations = 252 registrations)
git diff --check:              PASS
normal ctest:                  252/252 PASS
coverage ctest:                252/252 PASS
```

## 10. 遗留问题与后续边界

- Generation transform/emit 仍是 554/503/280/234 行级大函数，属于 P7 的 stage pipeline 重写对象；P1 只保证重写前有真实安全网；
- `ApiManager`、`ChannelManager`、session store 等仍由 service locator/singleton 获取，按 P5 注入；
- normalizer 的 schema-aware 复杂分支覆盖仍低；P7 拆 stage 时应按 alias、nested array、enum、strict client 分成 table-driven contract；
- AccountManager 仍同时承担选择、HTTP、workflow、存储协调和 worker，自动注册函数仍超过 300 行；P7-W2 才拆 selector/state machine/workers；
- Account fake 缝仅隔离网络和 sleep，不把 endpoint/retry/status 策略下沉到 adapter；
- 自动注册长轮询 deadline、并发注册、后台 worker、进程终止属于 P1-W5/P4；
- 本工作项不证明 SIGTERM、积压 drain 或客户端断连行为，不能据此通过阶段 1 总门禁；
- ASan 基础运行将在 P1-W5 汇总阶段 1 退出门禁时执行。

## 11. 回滚

- 测试安全网可通过删除两个 fixture test 和对应 CMake 条目独立回滚；
- Account seam 可删除 `AccountHttpTransport.*`/`AccountClock.*`，并把调用恢复为 Drogon/标准库直接调用，但会重新失去离线生命周期测试能力；
- Generation 权威收口若回滚，必须整体恢复成员声明、定义和 CMake，不能只恢复三份简化 helper，否则会重新产生 R1 同名竞争和“测试实现不等于生产实现”；
- 参数基础归一可独立回滚，但会恢复向客户端发出空/非法/非 object arguments 的历史风险，并使 characterization 测试失败。
