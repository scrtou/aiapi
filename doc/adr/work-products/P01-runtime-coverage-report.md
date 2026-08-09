# P1 · 运行时覆盖机器报告

> 此文件由 `tools/coverage/generate_report.py` 生成。不要手工修改数字。

## 1. 口径

- 数据源：测试进程运行产生的 `aiapi_legacy`/`aiapi_test` gcda；只统计仓库 `src/` 下生产文件。
- 生产 `.cpp/.cc` 只由 `aiapi_legacy` 编译一次；测试链接该库，不维护第二份生产源清单。
- 未进入测试链接对象图的文件标为 `not_instrumented`，不伪造可执行行分母，也不算入百分比。
- 分支采用 GCC gcov 分支口径，包含编译器生成的异常处理分支。

## 2. 摘要

| 项 | 值 |
|---|---:|
| production 实现文件 | 68 |
| 已编入并 instrument 的实现文件 | 53 |
| 未进入测试链接对象图的实现文件 | 15 |
| 行覆盖（仅已 instrument 实现） | 6028/11713 (51.46%) |
| 分支覆盖（仅已 instrument 实现） | 9210/34851 (26.43%) |
| gcda 文件 | 94 |
| 工具 | `gcov (Debian 12.2.0-14+deb12u1) 12.2.0` |

## 3. 高风险路径

| 路径 | 文件 | 状态 | 行 | 分支 |
|---|---|---|---:|---:|
| Chayns generate/postChatMessage | `src/apipoint/chaynsapi/chaynsapi.cpp` | `instrumented` | 543/1003 (54.14%) | 905/3232 (28.00%) |
| Generation ToolBridge transform/emit | `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp` | `instrumented` | 478/1102 (43.38%) | 743/3241 (22.93%) |
| Chat/Responses four GenerationService output modes | `src/sessionManager/core/GenerationService.cpp` | `instrumented` | 141/265 (53.21%) | 305/965 (31.61%) |
| Account selection/invalidation/rollback/pool rebuild | `src/accountManager/accountManager.cpp` | `instrumented` | 549/1489 (36.87%) | 1027/5735 (17.91%) |
| BackgroundTaskQueue shutdown/drain | `src/utils/BackgroundTaskQueue.h` | `instrumented` | 70/77 (90.91%) | 129/230 (56.09%) |
| Account worker interrupt/join | `src/accountManager/accountManager.cpp` | `instrumented` | 549/1489 (36.87%) | 1027/5735 (17.91%) |
| Session cleaner interrupt/join | `src/sessionManager/core/Session.cpp` | `instrumented` | 218/701 (31.10%) | 283/2084 (13.58%) |
| Chayns reaper interrupt/join | `src/apipoint/chaynsapi/chaynsThreadReaper.cpp` | `instrumented` | 40/109 (36.70%) | 35/234 (14.96%) |
| Streaming disconnect boundary | `src/utils/IoLoopResponseStream.h` | `instrumented` | 47/55 (85.45%) | 35/66 (53.03%) |
| HTTP Controller Chat/Responses route edge | `src/controllers/AiApiController.cc` | `not_instrumented` | n/a | n/a |
| Retool workflow/agent upstream paths | `src/apipoint/retoolapi/retoolapi.cpp` | `instrumented` | 406/889 (45.67%) | 677/3140 (21.56%) |
| Process shutdown orchestration after Drogon run | `src/utils/ApplicationShutdown.cpp` | `instrumented` | 17/17 (100.00%) | 65/98 (66.33%) |

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
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::setStatusTokenStatus(` | `executed` | 5 | 11/11 (100.00%) | 20/40 (50.00%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::rollbackWaitingAccount(` | `executed` | 1 | 4/4 (100.00%) | 2/4 (50.00%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::rebuildPoolLocked(` | `executed` | 11 | 11/12 (91.67%) | 16/26 (61.54%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::loadAccount(` | `executed` | 19 | 12/13 (92.31%) | 32/50 (64.00%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::addAccountbyPost(` | `executed` | 3 | 19/20 (95.00%) | 22/62 (35.48%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::updateAccount(` | `executed` | 3 | 21/22 (95.45%) | 22/42 (52.38%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::deleteAccountbyPost(` | `executed` | 2 | 10/10 (100.00%) | 17/30 (56.67%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::checkToken(` | `executed` | 1 | 18/23 (78.26%) | 68/136 (50.00%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::autoRegisterAccount(` | `executed` | 3 | 133/229 (58.08%) | 335/960 (34.90%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::enqueue(` | `executed` | 35 | 17/17 (100.00%) | 37/60 (61.67%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::shutdown(` | `executed` | 8 | 13/13 (100.00%) | 18/26 (69.23%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::workerLoop(` | `executed` | 11 | 16/23 (69.57%) | 48/98 (48.98%) |
| Account worker interrupt/join | `AccountManager::stopBackgroundThreads(` | `executed` | 43 | 18/18 (100.00%) | 21/32 (65.62%) |
| Session cleaner interrupt/join | `chatSession::stopClearExpiredSession(` | `executed` | 2 | 8/8 (100.00%) | 3/4 (75.00%) |
| Chayns reaper interrupt/join | `chaynsThreadReaper::stop(` | `executed` | 3 | 8/8 (100.00%) | 3/4 (75.00%) |
| Streaming disconnect boundary | `IoLoopResponseStream::send(` | `executed` | 4 | 11/12 (91.67%) | 6/12 (50.00%) |
| Streaming disconnect boundary | `IoLoopResponseStream::sendInLoop(` | `executed` | 3 | 5/6 (83.33%) | 5/8 (62.50%) |
| Streaming disconnect boundary | `IoLoopResponseStream::closeInLoop(` | `executed` | 4 | 7/7 (100.00%) | 4/6 (66.67%) |
| HTTP Controller Chat/Responses route edge | `AiApiController::chaynsapichat(` | `not_instrumented` | n/a | n/a | n/a |
| HTTP Controller Chat/Responses route edge | `AiApiController::responsesCreate(` | `not_instrumented` | n/a | n/a | n/a |
| Retool workflow/agent upstream paths | `retoolapi::requestWorkflow(` | `executed` | 8 | 60/75 (80.00%) | 125/344 (36.34%) |
| Retool workflow/agent upstream paths | `retoolapi::requestAgent(` | `executed` | 3 | 114/315 (36.19%) | 205/1288 (15.92%) |
| Process shutdown orchestration after Drogon run | `lifecycle::runApplicationShutdown(` | `executed` | 2 | 14/14 (100.00%) | 64/96 (66.67%) |

## 4. 未进入测试链接对象图的生产实现

这些文件没有运行时覆盖证据，是 P1 后续 fixture/characterization 的输入：

- `src/controllers/AccountController.cc`
- `src/controllers/AiApiController.cc`
- `src/controllers/ChannelController.cc`
- `src/controllers/LogController.cc`
- `src/controllers/MetricsController.cc`
- `src/controllers/RetoolWorkspaceController.cc`
- `src/dbManager/account/accountBackupDbManager.cpp`
- `src/dbManager/account/accountDbManager.cpp`
- `src/dbManager/channel/channelDbManager.cpp`
- `src/dbManager/chaynsThread/chaynsThreadDbManager.cpp`
- `src/dbManager/config/ConfigDbManager.cpp`
- `src/dbManager/metrics/StatusDbManager.cpp`
- `src/dbManager/retoolWorkspace/RetoolWorkspaceDbManager.cpp`
- `src/main.cc`
- `src/retoolWorkspace/RetoolWorkspaceService.cpp`

## 5. 已 instrument 实现明细

| 文件 | 状态 | 行 | 分支 |
|---|---|---:|---:|
| `src/accountManager/AccountClock.cpp` | `executed` | 2/5 (40.00%) | 1/2 (50.00%) |
| `src/accountManager/AccountHttpTransport.cpp` | `executed` | 2/6 (33.33%) | 1/6 (16.67%) |
| `src/accountManager/RetoolProvisionHealth.cpp` | `executed` | 49/50 (98.00%) | 50/86 (58.14%) |
| `src/accountManager/accountManager.cpp` | `executed` | 549/1489 (36.87%) | 1027/5735 (17.91%) |
| `src/apiManager/ApiFactory.cpp` | `executed` | 6/13 (46.15%) | 3/10 (30.00%) |
| `src/apiManager/ApiManager.cpp` | `executed` | 18/63 (28.57%) | 15/174 (8.62%) |
| `src/apipoint/chaynsapi/ChaynsClock.cpp` | `executed` | 6/6 (100.00%) | 1/2 (50.00%) |
| `src/apipoint/chaynsapi/ChaynsHttpTransport.cpp` | `instrumented_not_executed` | 0/6 (0.00%) | 0/6 (0.00%) |
| `src/apipoint/chaynsapi/ChaynsMessageCorrelation.cpp` | `executed` | 76/86 (88.37%) | 110/190 (57.89%) |
| `src/apipoint/chaynsapi/ChaynsModelCatalog.cpp` | `executed` | 197/219 (89.95%) | 294/556 (52.88%) |
| `src/apipoint/chaynsapi/chaynsThreadReaper.cpp` | `executed` | 40/109 (36.70%) | 35/234 (14.96%) |
| `src/apipoint/chaynsapi/chaynsapi.cpp` | `executed` | 543/1003 (54.14%) | 905/3232 (28.00%) |
| `src/apipoint/retoolapi/RetoolClock.cpp` | `instrumented_not_executed` | 0/6 (0.00%) | 0/2 (0.00%) |
| `src/apipoint/retoolapi/RetoolHttpTransport.cpp` | `instrumented_not_executed` | 0/6 (0.00%) | 0/6 (0.00%) |
| `src/apipoint/retoolapi/retoolapi.cpp` | `executed` | 406/889 (45.67%) | 677/3140 (21.56%) |
| `src/channelManager/channelManager.cpp` | `executed` | 75/116 (64.66%) | 100/248 (40.32%) |
| `src/controllers/HealthController.cc` | `executed` | 31/52 (59.62%) | 35/96 (36.46%) |
| `src/controllers/RetiredProviderTombstone.cc` | `executed` | 41/43 (95.35%) | 58/118 (49.15%) |
| `src/controllers/sinks/ChatJsonSink.cpp` | `executed` | 94/109 (86.24%) | 111/222 (50.00%) |
| `src/controllers/sinks/ChatSseSink.cpp` | `executed` | 95/186 (51.08%) | 138/520 (26.54%) |
| `src/controllers/sinks/ResponsesJsonSink.cpp` | `executed` | 101/132 (76.52%) | 132/334 (39.52%) |
| `src/controllers/sinks/ResponsesSseSink.cpp` | `executed` | 230/336 (68.45%) | 336/970 (34.64%) |
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
| `src/sessionManager/continuity/OutboundBudget.cpp` | `executed` | 46/68 (67.65%) | 34/112 (30.36%) |
| `src/sessionManager/continuity/ResponseIndex.cpp` | `executed` | 87/125 (69.60%) | 74/204 (36.27%) |
| `src/sessionManager/continuity/TextExtractor.cpp` | `executed` | 3/13 (23.08%) | 2/18 (11.11%) |
| `src/sessionManager/core/ClientOutputSanitizer.cpp` | `executed` | 7/46 (15.22%) | 6/168 (3.57%) |
| `src/sessionManager/core/GenerationService.cpp` | `executed` | 141/265 (53.21%) | 305/965 (31.61%) |
| `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp` | `executed` | 478/1102 (43.38%) | 743/3241 (22.93%) |
| `src/sessionManager/core/RequestAdapters.cpp` | `executed` | 540/792 (68.18%) | 1129/2996 (37.68%) |
| `src/sessionManager/core/RetiredProviderTelemetry.cpp` | `executed` | 3/3 (100.00%) | 9/18 (50.00%) |
| `src/sessionManager/core/Session.cpp` | `executed` | 218/701 (31.10%) | 283/2084 (13.58%) |
| `src/sessionManager/core/SessionCodec.cpp` | `instrumented_not_executed` | 0/137 (0.00%) | 0/382 (0.00%) |
| `src/sessionManager/tooling/BridgeHelpers.cpp` | `executed` | 98/103 (95.15%) | 118/200 (59.00%) |
| `src/sessionManager/tooling/BridgeProtocolCodec.cpp` | `executed` | 299/355 (84.23%) | 441/962 (45.84%) |
| `src/sessionManager/tooling/StrictClientRules.cpp` | `executed` | 98/113 (86.73%) | 114/220 (51.82%) |
| `src/sessionManager/tooling/ToolCallBridge.cpp` | `executed` | 21/149 (14.09%) | 6/262 (2.29%) |
| `src/sessionManager/tooling/ToolCallValidator.cpp` | `executed` | 268/357 (75.07%) | 488/1117 (43.69%) |
| `src/sessionManager/tooling/XmlTagToolCallCodec.cpp` | `executed` | 333/601 (55.41%) | 392/1309 (29.95%) |
| `src/tools/ZeroWidthEncoder.cpp` | `executed` | 58/117 (49.57%) | 42/115 (36.52%) |
| `src/utils/ApplicationShutdown.cpp` | `executed` | 17/17 (100.00%) | 65/98 (66.33%) |
| `src/utils/ConfigValidator.cpp` | `executed` | 67/153 (43.79%) | 166/646 (25.70%) |
