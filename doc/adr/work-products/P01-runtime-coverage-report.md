# P1 · 运行时覆盖机器报告

> 此文件由 `tools/coverage/generate_report.py` 生成。不要手工修改数字。

## 1. 口径

- 数据源：`aiapi_test` 运行产生的 gcda；只统计仓库 `src/` 下生产文件。
- 当前测试 target 直接重复编译部分生产 `.cpp/.cc`；本报告不把它表述为 production target 覆盖。
- 未编入测试 target 的文件标为 `not_instrumented`，不伪造可执行行分母，也不算入百分比。
- 分支采用 GCC gcov 分支口径，包含编译器生成的异常处理分支。

## 2. 摘要

| 项 | 值 |
|---|---:|
| production 实现文件 | 68 |
| 已编入并 instrument 的实现文件 | 50 |
| 未编入测试 target 的实现文件 | 18 |
| 行覆盖（仅已 instrument 实现） | 5800/11956 (48.51%) |
| 分支覆盖（仅已 instrument 实现） | 8807/36021 (24.45%) |
| gcda 文件 | 90 |
| 工具 | `gcov (Debian 12.2.0-14+deb12u1) 12.2.0` |

## 3. 高风险路径

| 路径 | 文件 | 状态 | 行 | 分支 |
|---|---|---|---:|---:|
| Chayns generate/postChatMessage | `src/apipoint/chaynsapi/chaynsapi.cpp` | `instrumented` | 543/1003 (54.14%) | 905/3232 (28.00%) |
| Generation ToolBridge transform/emit | `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp` | `instrumented` | 478/1102 (43.38%) | 743/3241 (22.93%) |
| Chat/Responses four GenerationService output modes | `src/sessionManager/core/GenerationService.cpp` | `instrumented` | 141/265 (53.21%) | 305/965 (31.61%) |
| Account selection/invalidation/rollback/pool rebuild | `src/accountManager/accountManager.cpp` | `instrumented` | 524/1765 (29.69%) | 983/6941 (14.16%) |
| BackgroundTaskQueue shutdown/drain | `src/utils/BackgroundTaskQueue.h` | `instrumented` | 57/64 (89.06%) | 105/196 (53.57%) |
| HTTP Controller Chat/Responses route edge | `src/controllers/AiApiController.cc` | `not_instrumented` | n/a | n/a |
| Retool workflow/agent upstream paths | `src/apipoint/retoolapi/retoolapi.cpp` | `instrumented` | 406/889 (45.67%) | 677/3140 (21.56%) |
| Process shutdown orchestration | `src/main.cc` | `not_instrumented` | n/a | n/a |

### 目标函数执行证据

| 路径 | 函数 | 状态 | 执行次数 | 行 | 分支 |
|---|---|---|---:|---:|---:|
| Chayns generate/postChatMessage | `chaynsapi::generate(` | `executed` | 9 | 23/27 (85.19%) | 24/48 (50.00%) |
| Chayns generate/postChatMessage | `chaynsapi::postChatMessage(` | `executed` | 9 | 404/634 (63.72%) | 752/2186 (34.40%) |
| Generation ToolBridge transform/emit | `toolcall::transformRequestForToolBridge(` | `executed` | 3 | 127/188 (67.55%) | 192/660 (29.09%) |
| Generation ToolBridge transform/emit | `GenerationService::emitResultEvents(` | `executed` | 8 | 134/245 (54.69%) | 202/718 (28.13%) |
| Generation ToolBridge transform/emit | `toolcall::generateForcedToolCall(` | `executed` | 7 | 78/140 (55.71%) | 143/498 (28.71%) |
| Generation ToolBridge transform/emit | `toolcall::normalizeToolCallArguments(` | `executed` | 13 | 48/161 (29.81%) | 81/678 (11.95%) |
| Chat/Responses four GenerationService output modes | `GenerationService::runGuarded(` | `executed` | 8 | 20/20 (100.00%) | 58/104 (55.77%) |
| Chat/Responses four GenerationService output modes | `GenerationService::executeProvider(` | `executed` | 8 | 22/30 (73.33%) | 40/106 (37.74%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::getAccount(` | `executed` | 6 | 33/36 (91.67%) | 83/168 (49.40%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::getEligibleAccount(` | `executed` | 9 | 55/68 (80.88%) | 69/140 (49.29%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::setStatusTokenStatus(` | `executed` | 5 | 11/11 (100.00%) | 21/42 (50.00%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::rollbackWaitingAccount(` | `executed` | 1 | 4/4 (100.00%) | 2/4 (50.00%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::rebuildPoolLocked(` | `executed` | 11 | 11/12 (91.67%) | 17/28 (60.71%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::loadAccount(` | `executed` | 18 | 12/13 (92.31%) | 32/50 (64.00%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::addAccountbyPost(` | `executed` | 2 | 16/17 (94.12%) | 16/52 (30.77%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::updateAccount(` | `executed` | 2 | 18/19 (94.74%) | 16/32 (50.00%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::deleteAccountbyPost(` | `executed` | 2 | 10/10 (100.00%) | 17/30 (56.67%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::checkToken(` | `executed` | 1 | 18/23 (78.26%) | 69/138 (50.00%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::autoRegisterAccount(` | `executed` | 2 | 133/236 (56.36%) | 332/986 (33.67%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::enqueue(` | `executed` | 6 | 17/17 (100.00%) | 37/60 (61.67%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::shutdown(` | `executed` | 4 | 13/13 (100.00%) | 18/26 (69.23%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::workerLoop(` | `executed` | 8 | 16/23 (69.57%) | 45/98 (45.92%) |
| HTTP Controller Chat/Responses route edge | `AiApiController::chaynsapichat(` | `not_instrumented` | n/a | n/a | n/a |
| HTTP Controller Chat/Responses route edge | `AiApiController::responsesCreate(` | `not_instrumented` | n/a | n/a | n/a |
| Retool workflow/agent upstream paths | `retoolapi::requestWorkflow(` | `executed` | 8 | 60/75 (80.00%) | 125/344 (36.34%) |
| Retool workflow/agent upstream paths | `retoolapi::requestAgent(` | `executed` | 3 | 114/315 (36.19%) | 205/1288 (15.92%) |
| Process shutdown orchestration | `main(` | `not_instrumented` | n/a | n/a | n/a |

## 4. 未编入测试 target 的生产实现

这些文件没有运行时覆盖证据，是 P1 后续 fixture/characterization 的输入：

- `src/apipoint/chaynsapi/chaynsThreadReaper.cpp`
- `src/apipoint/nexosapi/nexosapi.cpp`
- `src/apipoint/openai/OpenAiProvider.cpp`
- `src/controllers/AccountController.cc`
- `src/controllers/AiApiController.cc`
- `src/controllers/ChannelController.cc`
- `src/controllers/LogController.cc`
- `src/controllers/MetricsController.cc`
- `src/controllers/RetoolWorkspaceController.cc`
- `src/dbManager/account/accountBackupDbManager.cpp`
- `src/dbManager/account/accountDbManager.cpp`
- `src/dbManager/chaynsThread/chaynsThreadDbManager.cpp`
- `src/dbManager/config/ConfigDbManager.cpp`
- `src/dbManager/metrics/StatusDbManager.cpp`
- `src/dbManager/retoolWorkspace/RetoolWorkspaceDbManager.cpp`
- `src/main.cc`
- `src/retoolWorkspace/RetoolWorkspaceService.cpp`
- `src/tools/accountlogin/login_client.cpp`

## 5. 已 instrument 实现明细

| 文件 | 状态 | 行 | 分支 |
|---|---|---:|---:|
| `src/accountManager/AccountClock.cpp` | `executed` | 2/5 (40.00%) | 1/2 (50.00%) |
| `src/accountManager/AccountHttpTransport.cpp` | `executed` | 2/6 (33.33%) | 1/6 (16.67%) |
| `src/accountManager/RetoolProvisionHealth.cpp` | `executed` | 49/50 (98.00%) | 50/86 (58.14%) |
| `src/accountManager/accountManager.cpp` | `executed` | 524/1765 (29.69%) | 983/6941 (14.16%) |
| `src/apiManager/ApiFactory.cpp` | `executed` | 6/13 (46.15%) | 3/10 (30.00%) |
| `src/apiManager/ApiManager.cpp` | `executed` | 18/63 (28.57%) | 15/174 (8.62%) |
| `src/apipoint/chaynsapi/ChaynsClock.cpp` | `executed` | 6/6 (100.00%) | 1/2 (50.00%) |
| `src/apipoint/chaynsapi/ChaynsHttpTransport.cpp` | `instrumented_not_executed` | 0/6 (0.00%) | 0/6 (0.00%) |
| `src/apipoint/chaynsapi/ChaynsMessageCorrelation.cpp` | `executed` | 76/86 (88.37%) | 110/190 (57.89%) |
| `src/apipoint/chaynsapi/ChaynsModelCatalog.cpp` | `executed` | 197/219 (89.95%) | 294/556 (52.88%) |
| `src/apipoint/chaynsapi/chaynsapi.cpp` | `executed` | 543/1003 (54.14%) | 905/3232 (28.00%) |
| `src/apipoint/retoolapi/RetoolClock.cpp` | `instrumented_not_executed` | 0/6 (0.00%) | 0/2 (0.00%) |
| `src/apipoint/retoolapi/RetoolHttpTransport.cpp` | `instrumented_not_executed` | 0/6 (0.00%) | 0/6 (0.00%) |
| `src/apipoint/retoolapi/retoolapi.cpp` | `executed` | 406/889 (45.67%) | 677/3140 (21.56%) |
| `src/channelManager/channelManager.cpp` | `executed` | 58/98 (59.18%) | 83/208 (39.90%) |
| `src/controllers/HealthController.cc` | `executed` | 31/52 (59.62%) | 35/96 (36.46%) |
| `src/controllers/sinks/ChatJsonSink.cpp` | `executed` | 94/109 (86.24%) | 111/222 (50.00%) |
| `src/controllers/sinks/ChatSseSink.cpp` | `executed` | 95/186 (51.08%) | 138/520 (26.54%) |
| `src/controllers/sinks/ResponsesJsonSink.cpp` | `executed` | 101/132 (76.52%) | 132/334 (39.52%) |
| `src/controllers/sinks/ResponsesSseSink.cpp` | `executed` | 230/336 (68.45%) | 336/970 (34.64%) |
| `src/dbManager/channel/channelDbManager.cpp` | `instrumented_not_executed` | 0/196 (0.00%) | 0/648 (0.00%) |
| `src/dbManager/metrics/ErrorStatsDbManager.cpp` | `instrumented_not_executed` | 0/260 (0.00%) | 0/964 (0.00%) |
| `src/dbManager/session/SessionDbManager.cpp` | `instrumented_not_executed` | 0/265 (0.00%) | 0/848 (0.00%) |
| `src/managedAccount/backends/ClassicProviderAccountBackend.cpp` | `instrumented_not_executed` | 0/65 (0.00%) | 0/100 (0.00%) |
| `src/managedAccount/backends/RetoolWorkspaceBackend.cpp` | `executed` | 9/40 (22.50%) | 5/48 (10.42%) |
| `src/managedAccount/service/ManagedAccountService.cpp` | `executed` | 4/30 (13.33%) | 1/14 (7.14%) |
| `src/metrics/ErrorStatsConfig.cpp` | `executed` | 56/66 (84.85%) | 111/184 (60.33%) |
| `src/metrics/ErrorStatsService.cpp` | `executed` | 53/184 (28.80%) | 39/454 (8.59%) |
| `src/retoolWorkspace/RetoolWorkspaceManager.cpp` | `executed` | 50/58 (86.21%) | 17/38 (44.74%) |
| `src/sessionManager/actionProtocol/ActionProtocolAdapter.cpp` | `executed` | 33/35 (94.29%) | 39/66 (59.09%) |
| `src/sessionManager/actionProtocol/ActionProtocolCompiler.cpp` | `executed` | 245/285 (85.96%) | 284/596 (47.65%) |
| `src/sessionManager/continuity/ContinuityResolver.cpp` | `executed` | 88/97 (90.72%) | 102/201 (50.75%) |
| `src/sessionManager/continuity/HistoryReplayBudget.cpp` | `executed` | 147/181 (81.22%) | 166/332 (50.00%) |
| `src/sessionManager/continuity/OutboundBudget.cpp` | `executed` | 49/72 (68.06%) | 41/120 (34.17%) |
| `src/sessionManager/continuity/ResponseIndex.cpp` | `executed` | 87/125 (69.60%) | 74/204 (36.27%) |
| `src/sessionManager/continuity/TextExtractor.cpp` | `executed` | 3/13 (23.08%) | 2/18 (11.11%) |
| `src/sessionManager/core/ClientOutputSanitizer.cpp` | `executed` | 7/46 (15.22%) | 6/168 (3.57%) |
| `src/sessionManager/core/GenerationService.cpp` | `executed` | 141/265 (53.21%) | 305/965 (31.61%) |
| `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp` | `executed` | 478/1102 (43.38%) | 743/3241 (22.93%) |
| `src/sessionManager/core/RequestAdapters.cpp` | `executed` | 540/792 (68.18%) | 1129/2996 (37.68%) |
| `src/sessionManager/core/Session.cpp` | `executed` | 197/701 (28.10%) | 267/2084 (12.81%) |
| `src/sessionManager/core/SessionCodec.cpp` | `instrumented_not_executed` | 0/137 (0.00%) | 0/382 (0.00%) |
| `src/sessionManager/tooling/BridgeHelpers.cpp` | `executed` | 98/103 (95.15%) | 118/200 (59.00%) |
| `src/sessionManager/tooling/BridgeProtocolCodec.cpp` | `executed` | 299/355 (84.23%) | 441/962 (45.84%) |
| `src/sessionManager/tooling/StrictClientRules.cpp` | `executed` | 98/113 (86.73%) | 114/220 (51.82%) |
| `src/sessionManager/tooling/ToolCallBridge.cpp` | `executed` | 21/149 (14.09%) | 6/262 (2.29%) |
| `src/sessionManager/tooling/ToolCallValidator.cpp` | `executed` | 268/357 (75.07%) | 488/1117 (43.69%) |
| `src/sessionManager/tooling/XmlTagToolCallCodec.cpp` | `executed` | 333/601 (55.41%) | 392/1309 (29.95%) |
| `src/tools/ZeroWidthEncoder.cpp` | `executed` | 58/117 (49.57%) | 42/115 (36.52%) |
| `src/utils/ConfigValidator.cpp` | `instrumented_not_executed` | 0/110 (0.00%) | 0/462 (0.00%) |
