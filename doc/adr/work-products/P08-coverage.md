# 运行时覆盖机器报告

> 此文件由 `tools/coverage/generate_report.py` 生成。不要手工修改数字。

## 1. 口径

- 数据源：测试进程运行产生的 `aiapi_*` production libraries/`aiapi_test` gcda；只统计仓库 `src/` 下生产文件。
- 生产 `.cpp/.cc` 只由其 canonical production library 编译一次；测试链接这些 target，不维护第二份生产源清单。
- 未进入测试链接对象图的文件标为 `not_instrumented`，不伪造可执行行分母，也不算入百分比。
- 分支采用 GCC gcov 分支口径，包含编译器生成的异常处理分支。

## 2. 摘要

| 项 | 值 |
|---|---:|
| production 实现文件 | 89 |
| 已编入并 instrument 的实现文件 | 77 |
| 未进入测试链接对象图的实现文件 | 12 |
| 行覆盖（仅已 instrument 实现） | 7306/14614 (49.99%) |
| 分支覆盖（仅已 instrument 实现） | 9826/39050 (25.16%) |
| gcda 文件 | 142 |
| 工具 | `gcov (Debian 12.2.0-14+deb12u1) 12.2.0` |

## 3. 高风险路径

| 路径 | 文件 | 状态 | 行 | 分支 |
|---|---|---|---:|---:|
| Chayns provider generate/upstream request | `src/infrastructure/provider/chayns/chaynsapi.cpp` | `instrumented` | 569/983 (57.88%) | 957/3202 (29.89%) |
| Generation pipeline request/provider/commit | `src/sessionManager/core/GenerationPipeline.cpp` | `instrumented` | 257/365 (70.41%) | 350/953 (36.73%) |
| Generation response decode/normalize/emit | `src/sessionManager/core/GenerationResponsePipeline.cpp` | `instrumented` | 237/371 (63.88%) | 317/938 (33.80%) |
| Generation tool bridge request transform | `src/sessionManager/tooling/ToolDefinitionEncoder.cpp` | `instrumented` | 150/231 (64.94%) | 227/822 (27.62%) |
| Generation tool argument normalization | `src/sessionManager/tooling/ToolCallNormalizer.cpp` | `instrumented` | 62/164 (37.80%) | 76/642 (11.84%) |
| Generation forced tool fallback | `src/sessionManager/tooling/ForcedToolCallGenerator.cpp` | `instrumented` | 91/149 (61.07%) | 139/496 (28.02%) |
| Account selection/invalidation/pool rebuild | `src/accountManager/AccountSelector.cpp` | `instrumented` | 208/296 (70.27%) | 231/767 (30.12%) |
| Account registration rollback | `src/accountManager/AccountRegistrationWorkflow.cpp` | `instrumented` | 118/194 (60.82%) | 200/589 (33.96%) |
| Account token refresh | `src/accountManager/AccountTokenWorkflow.cpp` | `instrumented` | 37/181 (20.44%) | 48/569 (8.44%) |
| BackgroundTaskQueue shutdown/drain | `src/infrastructure/executor/BackgroundTaskQueue.h` | `instrumented` | 127/134 (94.78%) | 174/285 (61.05%) |
| Account worker interrupt/join | `src/accountManager/AccountWorkers.cpp` | `instrumented` | 55/90 (61.11%) | 26/118 (22.03%) |
| Session cleaner interrupt/join | `src/sessionManager/core/Session.cpp` | `instrumented` | 308/744 (41.40%) | 329/1668 (19.72%) |
| Chayns reaper interrupt/join | `src/infrastructure/provider/chayns/chaynsThreadReaper.cpp` | `instrumented` | 116/150 (77.33%) | 99/308 (32.14%) |
| Streaming disconnect boundary | `src/controllers/sinks/IoLoopResponseStream.h` | `instrumented` | 47/55 (85.45%) | 35/66 (53.03%) |
| HTTP Controller Chat/Responses route edge | `src/controllers/AiApiController.cc` | `instrumented` | 45/320 (14.06%) | 68/868 (7.83%) |
| Retool workflow/agent upstream paths | `src/infrastructure/provider/retool/retoolapi.cpp` | `instrumented` | 513/1011 (50.74%) | 811/3315 (24.46%) |
| Process shutdown orchestration after Drogon run | `src/runtime/AppContext.cpp` | `instrumented` | 55/55 (100.00%) | 106/184 (57.61%) |

### 目标函数执行证据

| 路径 | 函数 | 状态 | 执行次数 | 行 | 分支 |
|---|---|---|---:|---:|---:|
| Chayns provider generate/upstream request | `chaynsapi::doGenerate(` | `executed` | 22 | 406/635 (63.94%) | 768/2225 (34.52%) |
| Chayns provider generate/upstream request | `chaynsapi::sendWithinContext(` | `executed` | 40 | 6/7 (85.71%) | 5/10 (50.00%) |
| Generation pipeline request/provider/commit | `generation::GenerationPipeline::run(` | `executed` | 22 | 22/23 (95.65%) | 38/78 (48.72%) |
| Generation pipeline request/provider/commit | `generation::GenerationPipeline::execute(` | `executed` | 22 | 34/54 (62.96%) | 39/164 (23.78%) |
| Generation pipeline request/provider/commit | `generation::GenerationPipeline::invokeProvider(` | `executed` | 22 | 12/14 (85.71%) | 15/32 (46.88%) |
| Generation response decode/normalize/emit | `generation::GenerationResponsePipeline::emit(` | `executed` | 18 | 15/16 (93.75%) | 16/32 (50.00%) |
| Generation tool bridge request transform | `toolcall::transformRequestForToolBridge(` | `executed` | 8 | 44/59 (74.58%) | 74/202 (36.63%) |
| Generation tool argument normalization | `toolcall::normalizeToolCallArguments(` | `executed` | 28 | 23/24 (95.83%) | 35/78 (44.87%) |
| Generation forced tool fallback | `toolcall::generateForcedToolCall(` | `executed` | 16 | 35/36 (97.22%) | 52/92 (56.52%) |
| Account selection/invalidation/pool rebuild | `AccountManager::getAccount(` | `executed` | 12 | 28/32 (87.50%) | 32/70 (45.71%) |
| Account selection/invalidation/pool rebuild | `AccountManager::getEligibleAccount(` | `executed` | 20 | 35/45 (77.78%) | 41/99 (41.41%) |
| Account selection/invalidation/pool rebuild | `AccountManager::setStatusTokenStatus(` | `executed` | 10 | 18/18 (100.00%) | 15/30 (50.00%) |
| Account selection/invalidation/pool rebuild | `AccountManager::rebuildPoolLocked(` | `executed` | 56 | 12/12 (100.00%) | 17/28 (60.71%) |
| Account selection/invalidation/pool rebuild | `AccountManager::loadAccount(` | `executed` | 46 | 15/16 (93.75%) | 15/28 (53.57%) |
| Account selection/invalidation/pool rebuild | `AccountManager::addAccountbyPost(` | `executed` | 6 | 17/17 (100.00%) | 23/42 (54.76%) |
| Account selection/invalidation/pool rebuild | `AccountManager::updateAccount(` | `executed` | 6 | 23/25 (92.00%) | 16/30 (53.33%) |
| Account selection/invalidation/pool rebuild | `AccountManager::deleteAccountbyPost(` | `executed` | 4 | 13/14 (92.86%) | 8/14 (57.14%) |
| Account registration rollback | `AccountManager::rollbackWaitingAccount(` | `executed` | 2 | 4/4 (100.00%) | 1/2 (50.00%) |
| Account registration rollback | `AccountManager::autoRegisterAccount(` | `executed` | 6 | 80/148 (54.05%) | 151/471 (32.06%) |
| Account token refresh | `AccountManager::checkToken(` | `executed` | 2 | 15/18 (83.33%) | 23/46 (50.00%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::enqueue(` | `executed` | 254 | 21/21 (100.00%) | 40/68 (58.82%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::shutdown(` | `executed` | 64 | 1/1 (100.00%) | 1/2 (50.00%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::shutdown(` | `executed` | 6 | 2/2 (100.00%) | 1/2 (50.00%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::workerLoop(` | `executed` | 58 | 27/30 (90.00%) | 64/106 (60.38%) |
| Account worker interrupt/join | `AccountManager::stopBackgroundThreads(` | `executed` | 66 | 3/3 (100.00%) | 0/0 (n/a) |
| Account worker interrupt/join | `AccountManager::stopBackgroundThreads(` | `executed` | 66 | 24/27 (88.89%) | 12/30 (40.00%) |
| Session cleaner interrupt/join | `chatSession::stopClearExpiredSession(` | `executed` | 52 | 3/3 (100.00%) | 0/0 (n/a) |
| Session cleaner interrupt/join | `chatSession::stopClearExpiredSession(` | `executed` | 4 | 2/2 (100.00%) | 0/0 (n/a) |
| Chayns reaper interrupt/join | `chaynsThreadReaper::stop(` | `executed` | 14 | 3/3 (100.00%) | 1/2 (50.00%) |
| Chayns reaper interrupt/join | `chaynsThreadReaper::stop(` | `executed` | 8 | 2/2 (100.00%) | 1/2 (50.00%) |
| Streaming disconnect boundary | `IoLoopResponseStream::send(` | `executed` | 8 | 11/12 (91.67%) | 6/12 (50.00%) |
| Streaming disconnect boundary | `IoLoopResponseStream::sendInLoop(` | `executed` | 6 | 5/6 (83.33%) | 5/8 (62.50%) |
| Streaming disconnect boundary | `IoLoopResponseStream::closeInLoop(` | `executed` | 8 | 7/7 (100.00%) | 4/6 (66.67%) |
| HTTP Controller Chat/Responses route edge | `AiApiController::chaynsapichat(` | `not_executed` | 0 | 0/71 (0.00%) | 0/174 (0.00%) |
| HTTP Controller Chat/Responses route edge | `AiApiController::responsesCreate(` | `not_executed` | 0 | 0/77 (0.00%) | 0/202 (0.00%) |
| Retool workflow/agent upstream paths | `retoolapi::requestWorkflow(` | `executed` | 18 | 81/130 (62.31%) | 155/474 (32.70%) |
| Retool workflow/agent upstream paths | `retoolapi::requestAgent(` | `executed` | 6 | 139/379 (36.68%) | 238/1385 (17.18%) |
| Process shutdown orchestration after Drogon run | `lifecycle::AppContext::shutdown(` | `executed` | 34 | 6/6 (100.00%) | 10/14 (71.43%) |

## 4. 未进入测试链接对象图的生产实现

这些文件没有运行时覆盖证据，是后续 fixture/characterization 的输入：

- `src/accountManager/AccountClock.cpp`
- `src/controllers/LogController.cc`
- `src/controllers/RetoolWorkspaceController.cc`
- `src/dbManager/chaynsThread/chaynsThreadDbManager.cpp`
- `src/dbManager/config/ConfigDbManager.cpp`
- `src/infrastructure/account/DrogonAccountHttpTransport.cpp`
- `src/infrastructure/provider/chayns/ChaynsHttpTransport.cpp`
- `src/infrastructure/provider/retool/RetoolClock.cpp`
- `src/infrastructure/provider/retool/RetoolHttpTransport.cpp`
- `src/main.cc`
- `src/managedAccount/backends/ClassicProviderAccountBackend.cpp`
- `src/runtime/AppWiring.cpp`

## 5. 已 instrument 实现明细

| 文件 | 状态 | 行 | 分支 |
|---|---|---:|---:|
| `src/accountManager/AccountHealthWorkflow.cpp` | `executed` | 18/184 (9.78%) | 22/482 (4.56%) |
| `src/accountManager/AccountRegistrationStateMachine.cpp` | `executed` | 52/55 (94.55%) | 45/86 (52.33%) |
| `src/accountManager/AccountRegistrationWorkflow.cpp` | `executed` | 118/194 (60.82%) | 200/589 (33.96%) |
| `src/accountManager/AccountSelectionPolicy.cpp` | `executed` | 24/24 (100.00%) | 24/30 (80.00%) |
| `src/accountManager/AccountSelector.cpp` | `executed` | 208/296 (70.27%) | 231/767 (30.12%) |
| `src/accountManager/AccountTokenWorkflow.cpp` | `executed` | 37/181 (20.44%) | 48/569 (8.44%) |
| `src/accountManager/AccountWorkers.cpp` | `executed` | 55/90 (61.11%) | 26/118 (22.03%) |
| `src/accountManager/AccountWorkflowSupport.cpp` | `executed` | 61/100 (61.00%) | 105/338 (31.07%) |
| `src/accountManager/RetoolProvisionClock.cpp` | `executed` | 34/34 (100.00%) | 22/40 (55.00%) |
| `src/accountManager/RetoolProvisionHealth.cpp` | `executed` | 45/46 (97.83%) | 53/90 (58.89%) |
| `src/accountManager/accountManager.cpp` | `executed` | 35/173 (20.23%) | 27/214 (12.62%) |
| `src/application/account/AccountAdminUseCase.cpp` | `executed` | 16/83 (19.28%) | 12/184 (6.52%) |
| `src/application/channel/ChannelAdminUseCase.cpp` | `executed` | 47/91 (51.65%) | 31/106 (29.25%) |
| `src/application/health/HealthUseCase.cpp` | `executed` | 25/33 (75.76%) | 25/46 (54.35%) |
| `src/application/metrics/MetricsUseCase.cpp` | `executed` | 10/21 (47.62%) | 3/14 (21.43%) |
| `src/application/workspace/RetoolWorkspaceAdminUseCase.cpp` | `executed` | 10/28 (35.71%) | 0/0 (n/a) |
| `src/application/workspace/RetoolWorkspaceUseCase.cpp` | `executed` | 92/112 (82.14%) | 87/214 (40.65%) |
| `src/channelManager/channelManager.cpp` | `executed` | 67/120 (55.83%) | 71/212 (33.49%) |
| `src/controllers/AccountController.cc` | `executed` | 19/312 (6.09%) | 27/1028 (2.63%) |
| `src/controllers/AiApiController.cc` | `executed` | 45/320 (14.06%) | 68/868 (7.83%) |
| `src/controllers/ChannelController.cc` | `executed` | 38/144 (26.39%) | 69/562 (12.28%) |
| `src/controllers/HealthController.cc` | `executed` | 19/29 (65.52%) | 22/66 (33.33%) |
| `src/controllers/MetricsController.cc` | `executed` | 50/248 (20.16%) | 80/818 (9.78%) |
| `src/controllers/RetiredProviderTombstone.cc` | `executed` | 41/43 (95.35%) | 58/118 (49.15%) |
| `src/controllers/sinks/ChatJsonSink.cpp` | `executed` | 94/109 (86.24%) | 111/222 (50.00%) |
| `src/controllers/sinks/ChatSseSink.cpp` | `executed` | 95/186 (51.08%) | 138/520 (26.54%) |
| `src/controllers/sinks/ResponsesJsonSink.cpp` | `executed` | 107/136 (78.68%) | 144/360 (40.00%) |
| `src/controllers/sinks/ResponsesSseSink.cpp` | `executed` | 237/343 (69.10%) | 338/974 (34.70%) |
| `src/dbManager/account/accountBackupDbManager.cpp` | `instrumented_not_executed` | 0/96 (0.00%) | 0/254 (0.00%) |
| `src/dbManager/account/accountDbManager.cpp` | `instrumented_not_executed` | 0/350 (0.00%) | 0/1014 (0.00%) |
| `src/dbManager/channel/channelDbManager.cpp` | `instrumented_not_executed` | 0/217 (0.00%) | 0/696 (0.00%) |
| `src/dbManager/metrics/ErrorStatsDbManager.cpp` | `instrumented_not_executed` | 0/252 (0.00%) | 0/954 (0.00%) |
| `src/dbManager/metrics/StatusDbManager.cpp` | `instrumented_not_executed` | 0/327 (0.00%) | 0/752 (0.00%) |
| `src/dbManager/retoolWorkspace/RetoolWorkspaceDbManager.cpp` | `executed` | 9/226 (3.98%) | 4/474 (0.84%) |
| `src/dbManager/session/SessionDbManager.cpp` | `executed` | 37/289 (12.80%) | 44/878 (5.01%) |
| `src/infrastructure/config/ConfigValidator.cpp` | `executed` | 67/153 (43.79%) | 166/646 (25.70%) |
| `src/infrastructure/provider/ProviderBase.cpp` | `executed` | 29/33 (87.88%) | 27/46 (58.70%) |
| `src/infrastructure/provider/ProviderRegistry.cpp` | `executed` | 23/24 (95.83%) | 26/38 (68.42%) |
| `src/infrastructure/provider/chayns/ChaynsClock.cpp` | `executed` | 6/6 (100.00%) | 1/2 (50.00%) |
| `src/infrastructure/provider/chayns/ChaynsMessageCorrelation.cpp` | `executed` | 76/86 (88.37%) | 110/190 (57.89%) |
| `src/infrastructure/provider/chayns/ChaynsModelCatalog.cpp` | `executed` | 197/219 (89.95%) | 294/556 (52.88%) |
| `src/infrastructure/provider/chayns/chaynsThreadReaper.cpp` | `executed` | 116/150 (77.33%) | 99/308 (32.14%) |
| `src/infrastructure/provider/chayns/chaynsapi.cpp` | `executed` | 569/983 (57.88%) | 957/3202 (29.89%) |
| `src/infrastructure/provider/retool/retoolapi.cpp` | `executed` | 513/1011 (50.74%) | 811/3315 (24.46%) |
| `src/managedAccount/backends/RetoolWorkspaceBackend.cpp` | `executed` | 13/44 (29.55%) | 5/48 (10.42%) |
| `src/managedAccount/service/ManagedAccountService.cpp` | `executed` | 26/37 (70.27%) | 10/22 (45.45%) |
| `src/metrics/ErrorStatsConfig.cpp` | `executed` | 56/56 (100.00%) | 111/174 (63.79%) |
| `src/metrics/ErrorStatsService.cpp` | `executed` | 160/238 (67.23%) | 192/526 (36.50%) |
| `src/platform/ZeroWidthEncoder.cpp` | `executed` | 58/117 (49.57%) | 42/115 (36.52%) |
| `src/retoolWorkspace/RetoolWorkspaceManager.cpp` | `executed` | 48/58 (82.76%) | 15/34 (44.12%) |
| `src/retoolWorkspace/RetoolWorkspaceService.cpp` | `executed` | 6/114 (5.26%) | 2/348 (0.57%) |
| `src/runtime/AppContext.cpp` | `executed` | 55/55 (100.00%) | 106/184 (57.61%) |
| `src/sessionManager/actionProtocol/ActionProtocolAdapter.cpp` | `executed` | 33/35 (94.29%) | 39/66 (59.09%) |
| `src/sessionManager/actionProtocol/ActionProtocolCompiler.cpp` | `executed` | 245/285 (85.96%) | 284/596 (47.65%) |
| `src/sessionManager/continuity/ContinuityResolver.cpp` | `executed` | 85/94 (90.43%) | 100/197 (50.76%) |
| `src/sessionManager/continuity/HistoryReplayBudget.cpp` | `executed` | 147/181 (81.22%) | 165/332 (49.70%) |
| `src/sessionManager/continuity/OutboundBudget.cpp` | `executed` | 46/68 (67.65%) | 34/112 (30.36%) |
| `src/sessionManager/continuity/ResponseIndex.cpp` | `executed` | 125/139 (89.93%) | 113/232 (48.71%) |
| `src/sessionManager/continuity/TextExtractor.cpp` | `executed` | 3/13 (23.08%) | 2/18 (11.11%) |
| `src/sessionManager/core/AiApiUseCase.cpp` | `executed` | 41/192 (21.35%) | 24/267 (8.99%) |
| `src/sessionManager/core/ClientOutputSanitizer.cpp` | `executed` | 7/46 (15.22%) | 6/158 (3.80%) |
| `src/sessionManager/core/GenerationPipeline.cpp` | `executed` | 257/365 (70.41%) | 350/953 (36.73%) |
| `src/sessionManager/core/GenerationResponsePipeline.cpp` | `executed` | 237/371 (63.88%) | 317/938 (33.80%) |
| `src/sessionManager/core/GenerationService.cpp` | `executed` | 8/8 (100.00%) | 0/0 (n/a) |
| `src/sessionManager/core/RequestAdapters.cpp` | `executed` | 538/787 (68.36%) | 1002/2736 (36.62%) |
| `src/sessionManager/core/RetiredProviderTelemetry.cpp` | `executed` | 2/23 (8.70%) | 1/62 (1.61%) |
| `src/sessionManager/core/Session.cpp` | `executed` | 308/744 (41.40%) | 329/1668 (19.72%) |
| `src/sessionManager/core/SessionCodec.cpp` | `instrumented_not_executed` | 0/137 (0.00%) | 0/382 (0.00%) |
| `src/sessionManager/tooling/BridgeHelpers.cpp` | `executed` | 127/132 (96.21%) | 146/278 (52.52%) |
| `src/sessionManager/tooling/BridgeProtocolCodec.cpp` | `executed` | 309/355 (87.04%) | 450/962 (46.78%) |
| `src/sessionManager/tooling/ForcedToolCallGenerator.cpp` | `executed` | 91/149 (61.07%) | 139/496 (28.02%) |
| `src/sessionManager/tooling/StrictClientRules.cpp` | `executed` | 98/113 (86.73%) | 112/216 (51.85%) |
| `src/sessionManager/tooling/ToolCallBridge.cpp` | `executed` | 21/149 (14.09%) | 6/262 (2.29%) |
| `src/sessionManager/tooling/ToolCallNormalizer.cpp` | `executed` | 62/164 (37.80%) | 76/642 (11.84%) |
| `src/sessionManager/tooling/ToolCallValidator.cpp` | `executed` | 270/356 (75.84%) | 428/995 (43.02%) |
| `src/sessionManager/tooling/ToolDefinitionEncoder.cpp` | `executed` | 150/231 (64.94%) | 227/822 (27.62%) |
| `src/sessionManager/tooling/XmlTagToolCallCodec.cpp` | `executed` | 333/601 (55.41%) | 367/1249 (29.38%) |
