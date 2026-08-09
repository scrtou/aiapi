# 架构审计基线

> 本文件由 `tools/architecture_audit.py` 生成，请勿手改数字。机器可读真值为 [`audit-baseline.json`](./audit-baseline.json)。

## 1. 快照

| 项 | 值 |
|---|---|
| schema | v3 |
| 生成时间 | `2026-08-09T02:51:48+00:00` |
| 基础 commit | `7d7883a` |
| 工作区 | **dirty（实施快照，不是可复现发布基线）** |
| 扫描扩展名 | `.h/.hpp/.cpp/.cc` |

发布 tag 使用的基线必须来自 `git.dirty=false`。dirty 基线只允许作为当前实施快照。

## 2. 指标

| 规则 | 当前值 | 含义 |
|---|---:|---|
| R1 | **4** | tooling/actionProtocol 自由函数与 GenerationService 成员同名竞争 |
| R2 | **43** | fan-in ≥2 且无直接/显式 test owner 的生产头；**不是运行时覆盖** |
| R3 | **13** | `.cpp/.cc` 中超过 200 行的函数候选 |
| R3 行数 | **5004** | R3 候选总行数 |

| 规模项 | 当前值 |
|---|---:|
| 生产翻译单元 | 65 |
| 测试直接编译的生产源 | 38 |
| 测试源 | 35 |
| 测试用例 | 223 |
| 静态断言宏计数 | 869 |

规模项用于发现 CMake/注册漂移，不作为质量 KPI。

## 3. R2 语义

owner 只来自有用例和断言的测试源直接 include，或 `// ARCH_TESTS: path/from/src/Header.h`。传递 include 不产生 owner。即使有 owner，也不能证明运行时执行；真实覆盖使用 gcov/llvm-cov。

v3 口径与旧版不可比较，当前值是新棘轮起点。无行为头可以通过规则修正或 ADR 排除，不应为清零写无意义断言。

## 4. R2 明细

| fan-in | impl 行 | 已进测试链接 | 头文件 |
|---:|---:|:---:|---|
| 11 | 0 | 否 | `src/domain/model/SessionData.h` |
| 10 | 115 | 是 | `src/apiManager/ApiManager.h` |
| 9 | 0 | 否 | `src/sessionManager/contracts/GenerationEvent.h` |
| 7 | 28 | 是 | `src/apiManager/ApiFactory.h` |
| 6 | 0 | 否 | `src/controllers/AdminAuthFilter.h` |
| 6 | 0 | 否 | `src/sessionManager/contracts/GenerationRequest.h` |
| 5 | 591 | 否 | `src/dbManager/account/accountDbManager.h` |
| 5 | 351 | 是 | `src/metrics/ErrorStatsService.h` |
| 5 | 0 | 否 | `src/domain/model/RetoolWorkspaceInfo.h` |
| 5 | 0 | 否 | `src/domain/port/APIinterface.h` |
| 4 | 490 | 是 | `src/dbManager/session/SessionDbManager.h` |
| 4 | 383 | 否 | `src/dbManager/chaynsThread/chaynsThreadDbManager.h` |
| 4 | 162 | 否 | `src/dbManager/account/accountBackupDbManager.h` |
| 4 | 87 | 否 | `src/sessionManager/core/ClientOutputSanitizer.h` |
| 4 | 0 | 否 | `src/apiManager/Apicomn.h` |
| 4 | 0 | 否 | `src/dbManager/DbType.h` |
| 4 | 0 | 否 | `src/domain/model/AccountData.h` |
| 4 | 0 | 否 | `src/domain/model/ProviderResult.h` |
| 4 | 0 | 否 | `src/sessionManager/tooling/ToolDefinitionResolver.h` |
| 3 | 533 | 否 | `src/sessionManager/core/GenerationService.h` |
| 3 | 384 | 是 | `src/dbManager/channel/channelDbManager.h` |
| 3 | 181 | 否 | `src/retoolWorkspace/RetoolWorkspaceService.h` |
| 3 | 142 | 否 | `src/dbManager/config/ConfigDbManager.h` |
| 3 | 61 | 否 | `src/managedAccount/service/ManagedAccountService.h` |
| 3 | 30 | 否 | `src/sessionManager/tooling/ToolDefinitionEncoder.h` |
| 2 | 1442 | 否 | `src/apipoint/chaynsapi/chaynsapi.h` |
| 2 | 1336 | 否 | `src/apipoint/nexosapi/nexosapi.h` |
| 2 | 523 | 是 | `src/dbManager/metrics/ErrorStatsDbManager.h` |
| 2 | 452 | 否 | `src/dbManager/retoolWorkspace/RetoolWorkspaceDbManager.h` |
| 2 | 206 | 是 | `src/sessionManager/core/SessionCodec.h` |
| 2 | 185 | 否 | `src/apipoint/chaynsapi/chaynsThreadReaper.h` |
| 2 | 184 | 是 | `src/utils/ConfigValidator.h` |
| 2 | 102 | 否 | `src/managedAccount/backends/ClassicProviderAccountBackend.h` |
| 2 | 62 | 否 | `src/managedAccount/backends/RetoolWorkspaceBackend.h` |
| 2 | 22 | 是 | `src/sessionManager/continuity/TextExtractor.h` |
| 2 | 0 | 否 | `src/controllers/RateLimitFilter.h` |
| 2 | 0 | 否 | `src/domain/model/BridgeWireFormat.h` |
| 2 | 0 | 否 | `src/domain/model/ChannelInfo.h` |
| 2 | 0 | 否 | `src/domain/model/ImageInfo.h` |
| 2 | 0 | 否 | `src/managedAccount/backends/IManagedAccountBackend.h` |
| 2 | 0 | 否 | `src/managedAccount/contracts/ManagedAccount.h` |
| 2 | 0 | 否 | `src/sessionManager/core/Errors.h` |
| 2 | 0 | 否 | `src/sessionManager/core/SessionExecutionGate.h` |

## 5. R3 明细

| 行数 | 位置 |
|---:|---|
| 865 | `src/apipoint/chaynsapi/chaynsapi.cpp:284-1148` |
| 554 | `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp:1661-2214` |
| 503 | `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp:516-1018` |
| 459 | `src/apipoint/retoolapi/retoolapi.cpp:847-1305` |
| 375 | `src/sessionManager/tooling/XmlTagToolCallCodec.cpp:678-1052` |
| 334 | `src/sessionManager/tooling/XmlTagToolCallCodec.cpp:344-677` |
| 325 | `src/accountManager/accountManager.cpp:1937-2261` |
| 313 | `src/main.cc:121-433` |
| 283 | `src/sessionManager/core/GenerationService.cpp:143-425` |
| 272 | `src/sessionManager/tooling/BridgeProtocolCodec.cpp:170-441` |
| 258 | `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp:1386-1643` |
| 234 | `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp:1152-1385` |
| 229 | `src/sessionManager/core/RequestAdapters.cpp:796-1024` |

## 6. 更新

```bash
python3 tools/architecture_audit.py --selftest
python3 tools/architecture_audit.py \
  --write-baseline doc/adr/audit-baseline.json \
  --write-markdown doc/adr/architecture-baseline.md
```

回归检查：

```bash
python3 tools/architecture_audit.py --baseline doc/adr/audit-baseline.json
```
