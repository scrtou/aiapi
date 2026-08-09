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
| production 实现文件 | 67 |
| 已编入并 instrument 的实现文件 | 41 |
| 未编入测试 target 的实现文件 | 26 |
| 行覆盖（仅已 instrument 实现） | 4228/9541 (44.31%) |
| 分支覆盖（仅已 instrument 实现） | 6133/28429 (21.57%) |
| gcda 文件 | 78 |
| 工具 | `gcov (Debian 12.2.0-14+deb12u1) 12.2.0` |

## 3. 高风险路径

| 路径 | 文件 | 状态 | 行 | 分支 |
|---|---|---|---:|---:|
| Chayns generate/postChatMessage | `src/apipoint/chaynsapi/chaynsapi.cpp` | `instrumented` | 529/1003 (52.74%) | 871/3232 (26.95%) |
| Generation ToolBridge transform/emit | `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp` | `not_instrumented` | n/a | n/a |
| Account selection/invalidation/rollback/pool rebuild | `src/accountManager/accountManager.cpp` | `instrumented` | 247/1760 (14.03%) | 396/6937 (5.71%) |
| BackgroundTaskQueue shutdown/drain | `src/utils/BackgroundTaskQueue.h` | `instrumented` | 57/64 (89.06%) | 108/196 (55.10%) |
| Chat/Responses streaming and JSON entry paths | `src/controllers/AiApiController.cc` | `not_instrumented` | n/a | n/a |
| Retool workflow/agent upstream paths | `src/apipoint/retoolapi/retoolapi.cpp` | `not_instrumented` | n/a | n/a |
| Process shutdown orchestration | `src/main.cc` | `not_instrumented` | n/a | n/a |

### 目标函数执行证据

| 路径 | 函数 | 状态 | 执行次数 | 行 | 分支 |
|---|---|---|---:|---:|---:|
| Chayns generate/postChatMessage | `chaynsapi::generate(` | `executed` | 5 | 23/27 (85.19%) | 24/48 (50.00%) |
| Chayns generate/postChatMessage | `chaynsapi::postChatMessage(` | `executed` | 5 | 404/634 (63.72%) | 752/2186 (34.40%) |
| Generation ToolBridge transform/emit | `GenerationService::transformRequestForToolBridge(` | `not_instrumented` | n/a | n/a | n/a |
| Generation ToolBridge transform/emit | `GenerationService::emitResultEvents(` | `not_instrumented` | n/a | n/a | n/a |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::getAccount(` | `executed` | 6 | 33/36 (91.67%) | 83/168 (49.40%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::getEligibleAccount(` | `executed` | 4 | 47/68 (69.12%) | 53/140 (37.86%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::setStatusTokenStatus(` | `executed` | 4 | 11/11 (100.00%) | 21/42 (50.00%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::rollbackWaitingAccount(` | `not_executed` | 0 | 0/4 (0.00%) | 0/4 (0.00%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::rebuildPoolLocked(` | `executed` | 8 | 9/12 (75.00%) | 14/28 (50.00%) |
| Account selection/invalidation/rollback/pool rebuild | `AccountManager::loadAccount(` | `executed` | 11 | 12/13 (92.31%) | 32/50 (64.00%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::enqueue(` | `executed` | 6 | 17/17 (100.00%) | 37/60 (61.67%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::shutdown(` | `executed` | 4 | 13/13 (100.00%) | 18/26 (69.23%) |
| BackgroundTaskQueue shutdown/drain | `BackgroundTaskQueue::workerLoop(` | `executed` | 8 | 16/23 (69.57%) | 48/98 (48.98%) |
| Chat/Responses streaming and JSON entry paths | `AiApiController::chaynsapichat(` | `not_instrumented` | n/a | n/a | n/a |
| Chat/Responses streaming and JSON entry paths | `AiApiController::responsesCreate(` | `not_instrumented` | n/a | n/a | n/a |
| Retool workflow/agent upstream paths | `retoolapi::requestWorkflow(` | `not_instrumented` | n/a | n/a | n/a |
| Retool workflow/agent upstream paths | `retoolapi::requestAgent(` | `not_instrumented` | n/a | n/a | n/a |
| Process shutdown orchestration | `main(` | `not_instrumented` | n/a | n/a | n/a |

## 4. 未编入测试 target 的生产实现

这些文件没有运行时覆盖证据，是 P1 后续 fixture/characterization 的输入：

- `src/apipoint/chaynsapi/chaynsThreadReaper.cpp`
- `src/apipoint/nexosapi/nexosapi.cpp`
- `src/apipoint/openai/OpenAiProvider.cpp`
- `src/apipoint/retoolapi/retoolapi.cpp`
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
- `src/managedAccount/backends/ClassicProviderAccountBackend.cpp`
- `src/managedAccount/backends/RetoolWorkspaceBackend.cpp`
- `src/managedAccount/service/ManagedAccountService.cpp`
- `src/retoolWorkspace/RetoolWorkspaceService.cpp`
- `src/sessionManager/core/ClientOutputSanitizer.cpp`
- `src/sessionManager/core/GenerationService.cpp`
- `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp`
- `src/sessionManager/tooling/ToolDefinitionEncoder.cpp`
- `src/tools/accountlogin/login_client.cpp`

## 5. 已 instrument 实现明细

| 文件 | 状态 | 行 | 分支 |
|---|---|---:|---:|
| `src/accountManager/RetoolProvisionHealth.cpp` | `executed` | 49/50 (98.00%) | 50/86 (58.14%) |
| `src/accountManager/accountManager.cpp` | `executed` | 247/1760 (14.03%) | 396/6937 (5.71%) |
| `src/apiManager/ApiFactory.cpp` | `executed` | 6/13 (46.15%) | 2/10 (20.00%) |
| `src/apiManager/ApiManager.cpp` | `executed` | 12/63 (19.05%) | 9/174 (5.17%) |
| `src/apipoint/chaynsapi/ChaynsClock.cpp` | `executed` | 6/6 (100.00%) | 1/2 (50.00%) |
| `src/apipoint/chaynsapi/ChaynsHttpTransport.cpp` | `instrumented_not_executed` | 0/6 (0.00%) | 0/6 (0.00%) |
| `src/apipoint/chaynsapi/ChaynsMessageCorrelation.cpp` | `executed` | 76/86 (88.37%) | 110/190 (57.89%) |
| `src/apipoint/chaynsapi/ChaynsModelCatalog.cpp` | `executed` | 197/219 (89.95%) | 294/556 (52.88%) |
| `src/apipoint/chaynsapi/chaynsapi.cpp` | `executed` | 529/1003 (52.74%) | 871/3232 (26.95%) |
| `src/channelManager/channelManager.cpp` | `executed` | 50/98 (51.02%) | 77/208 (37.02%) |
| `src/controllers/HealthController.cc` | `executed` | 31/52 (59.62%) | 35/96 (36.46%) |
| `src/controllers/sinks/ChatJsonSink.cpp` | `executed` | 90/109 (82.57%) | 104/222 (46.85%) |
| `src/controllers/sinks/ChatSseSink.cpp` | `executed` | 65/186 (34.95%) | 77/520 (14.81%) |
| `src/controllers/sinks/ResponsesJsonSink.cpp` | `executed` | 97/132 (73.48%) | 125/334 (37.43%) |
| `src/controllers/sinks/ResponsesSseSink.cpp` | `executed` | 158/336 (47.02%) | 244/970 (25.15%) |
| `src/dbManager/channel/channelDbManager.cpp` | `instrumented_not_executed` | 0/196 (0.00%) | 0/648 (0.00%) |
| `src/dbManager/metrics/ErrorStatsDbManager.cpp` | `instrumented_not_executed` | 0/260 (0.00%) | 0/964 (0.00%) |
| `src/dbManager/session/SessionDbManager.cpp` | `instrumented_not_executed` | 0/265 (0.00%) | 0/848 (0.00%) |
| `src/metrics/ErrorStatsConfig.cpp` | `executed` | 56/66 (84.85%) | 111/184 (60.33%) |
| `src/metrics/ErrorStatsService.cpp` | `instrumented_not_executed` | 0/184 (0.00%) | 0/454 (0.00%) |
| `src/retoolWorkspace/RetoolWorkspaceManager.cpp` | `executed` | 50/58 (86.21%) | 17/38 (44.74%) |
| `src/sessionManager/actionProtocol/ActionProtocolAdapter.cpp` | `executed` | 33/35 (94.29%) | 39/66 (59.09%) |
| `src/sessionManager/actionProtocol/ActionProtocolCompiler.cpp` | `executed` | 240/285 (84.21%) | 278/596 (46.64%) |
| `src/sessionManager/continuity/ContinuityResolver.cpp` | `executed` | 88/97 (90.72%) | 102/201 (50.75%) |
| `src/sessionManager/continuity/HistoryReplayBudget.cpp` | `executed` | 147/181 (81.22%) | 166/332 (50.00%) |
| `src/sessionManager/continuity/OutboundBudget.cpp` | `executed` | 49/72 (68.06%) | 41/120 (34.17%) |
| `src/sessionManager/continuity/ResponseIndex.cpp` | `executed` | 87/125 (69.60%) | 73/204 (35.78%) |
| `src/sessionManager/continuity/TextExtractor.cpp` | `executed` | 3/13 (23.08%) | 2/18 (11.11%) |
| `src/sessionManager/core/RequestAdapters.cpp` | `executed` | 540/792 (68.18%) | 1129/2996 (37.68%) |
| `src/sessionManager/core/Session.cpp` | `executed` | 172/701 (24.54%) | 228/2084 (10.94%) |
| `src/sessionManager/core/SessionCodec.cpp` | `instrumented_not_executed` | 0/137 (0.00%) | 0/382 (0.00%) |
| `src/sessionManager/tooling/BridgeHelpers.cpp` | `executed` | 82/103 (79.61%) | 109/200 (54.50%) |
| `src/sessionManager/tooling/BridgeProtocolCodec.cpp` | `executed` | 252/355 (70.99%) | 360/962 (37.42%) |
| `src/sessionManager/tooling/ForcedToolCallGenerator.cpp` | `executed` | 27/27 (100.00%) | 34/58 (58.62%) |
| `src/sessionManager/tooling/StrictClientRules.cpp` | `executed` | 98/113 (86.73%) | 114/220 (51.82%) |
| `src/sessionManager/tooling/ToolCallBridge.cpp` | `executed` | 21/149 (14.09%) | 6/262 (2.29%) |
| `src/sessionManager/tooling/ToolCallNormalizer.cpp` | `executed` | 23/23 (100.00%) | 30/46 (65.22%) |
| `src/sessionManager/tooling/ToolCallValidator.cpp` | `executed` | 256/357 (71.71%) | 465/1117 (41.63%) |
| `src/sessionManager/tooling/XmlTagToolCallCodec.cpp` | `executed` | 333/601 (55.41%) | 392/1309 (29.95%) |
| `src/tools/ZeroWidthEncoder.cpp` | `executed` | 58/117 (49.57%) | 42/115 (36.52%) |
| `src/utils/ConfigValidator.cpp` | `instrumented_not_executed` | 0/110 (0.00%) | 0/462 (0.00%) |
