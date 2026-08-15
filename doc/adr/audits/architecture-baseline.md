# 架构审计基线

> 本文件由 `tools/architecture_audit.py` 生成，请勿手改数字。机器可读真值为 [`audit-baseline.json`](./audit-baseline.json)。

## 1. 快照

| 项 | 值 |
|---|---|
| schema | v4 |
| 生成时间 | `2026-08-15T08:49:10+00:00` |
| 基础 commit | `c0f8fc5` |
| 工作区 | **clean** |
| 扫描扩展名 | `.h/.hpp/.cpp/.cc` |

发布 tag 使用的基线必须来自 `git.dirty=false`。dirty 基线只允许作为当前实施快照。

## 2. 指标

| 规则 | 当前值 | 含义 |
|---|---:|---|
| R1 | **0** | tooling/actionProtocol 自由函数与 GenerationService 成员同名竞争 |
| R2 | **41** | fan-in ≥2 且无直接/显式 test owner 的生产头；**不是运行时覆盖** |
| R3 | **6** | `.cpp/.cc` 中超过 200 行的函数候选 |
| R3 行数 | **2563** | R3 候选总行数 |

| 规模项 | 当前值 |
|---|---:|
| 生产翻译单元 | 89 |
| 已登记 production library owner 的生产源 | 88 |
| 测试源 | 65 |
| 测试用例 | 396 |
| 静态断言宏计数 | 1755 |

规模项用于发现 CMake/注册漂移，不作为质量 KPI。

## 3. R2 语义

owner 只来自有用例和断言的测试源直接 include，或 `// ARCH_TESTS: path/from/src/Header.h`。传递 include 不产生 owner。即使有 owner，也不能证明运行时执行；真实覆盖使用 gcov/llvm-cov。

v4 不再把 production source list 称为‘已进测试链接’：普通链接静态库时，源属于库不能证明该 object 已被 linker 提取。运行时真值使用 gcov。无行为头可以通过规则修正或 ADR 排除，不应为清零写无意义断言。

## 4. R2 明细

| fan-in | impl 行 | 已登记 production owner | 头文件 |
|---:|---:|:---:|---|
| 21 | 0 | 否 | `src/platform/Log.h` |
| 13 | 0 | 否 | `src/domain/model/AccountData.h` |
| 9 | 0 | 否 | `src/application/generation/contracts/GenerationEvent.h` |
| 9 | 0 | 否 | `src/application/generation/contracts/LegacySessionData.h` |
| 6 | 0 | 否 | `src/application/generation/contracts/GenerationRequest.h` |
| 6 | 0 | 否 | `src/application/generation/tooling/ToolDefinitionResolver.h` |
| 6 | 0 | 否 | `src/transport/controllers/AdminAuthFilter.h` |
| 5 | 416 | 是 | `src/infrastructure/persistence/chaynsThread/chaynsThreadDbManager.h` |
| 5 | 0 | 否 | `src/domain/port/IResponseIndex.h` |
| 5 | 0 | 否 | `src/infrastructure/persistence/DbType.h` |
| 4 | 0 | 否 | `src/infrastructure/managedAccount/contracts/ManagedAccount.h` |
| 4 | 0 | 否 | `src/platform/result/Error.h` |
| 3 | 41 | 是 | `src/application/generation/core/RetiredProviderTelemetry.h` |
| 3 | 0 | 否 | `src/domain/model/ImageInfo.h` |
| 3 | 0 | 否 | `src/domain/model/ProviderModelCatalog.h` |
| 3 | 0 | 否 | `src/domain/model/RequestAggData.h` |
| 3 | 0 | 否 | `src/domain/port/IExecutionGate.h` |
| 3 | 0 | 否 | `src/domain/port/IProviderModelCatalog.h` |
| 3 | 0 | 否 | `src/domain/port/IProviderThreadContext.h` |
| 3 | 0 | 否 | `src/domain/port/IRetoolWorkspaceAdminUseCase.h` |
| 3 | 0 | 否 | `src/domain/port/ITelemetrySink.h` |
| 3 | 0 | 否 | `src/infrastructure/managedAccount/backends/IManagedAccountBackend.h` |
| 2 | 746 | 是 | `src/runtime/AppWiring.h` |
| 2 | 588 | 是 | `src/application/generation/core/GenerationPipeline.h` |
| 2 | 556 | 是 | `src/application/generation/core/GenerationResponsePipeline.h` |
| 2 | 360 | 是 | `src/application/generation/tooling/ToolDefinitionEncoder.h` |
| 2 | 289 | 是 | `src/transport/controllers/RetoolWorkspaceController.h` |
| 2 | 206 | 是 | `src/application/generation/core/SessionCodec.h` |
| 2 | 134 | 是 | `src/infrastructure/managedAccount/backends/ClassicProviderAccountBackend.h` |
| 2 | 86 | 是 | `src/application/generation/core/ClientOutputSanitizer.h` |
| 2 | 57 | 是 | `src/infrastructure/account/DrogonAccountHttpTransport.h` |
| 2 | 22 | 是 | `src/application/generation/continuity/TextExtractor.h` |
| 2 | 0 | 否 | `src/domain/model/BridgeWireFormat.h` |
| 2 | 0 | 否 | `src/domain/port/IAccountSelector.h` |
| 2 | 0 | 否 | `src/domain/port/IAccountSettingsQuery.h` |
| 2 | 0 | 否 | `src/domain/port/IChatProvider.h` |
| 2 | 0 | 否 | `src/platform/Deadline.h` |
| 2 | 0 | 否 | `src/platform/LocalDateTime.h` |
| 2 | 0 | 否 | `src/platform/Uuid.h` |
| 2 | 0 | 否 | `src/platform/result/ErrorCode.h` |
| 2 | 0 | 否 | `src/transport/controllers/RateLimitFilter.h` |

## 5. R3 明细

| 行数 | 位置 |
|---:|---|
| 893 | `src/infrastructure/provider/chayns/chaynsapi.cpp:328-1220` |
| 460 | `src/infrastructure/provider/retool/retoolapi.cpp:955-1414` |
| 375 | `src/application/generation/tooling/XmlTagToolCallCodec.cpp:677-1051` |
| 334 | `src/application/generation/tooling/XmlTagToolCallCodec.cpp:343-676` |
| 272 | `src/application/generation/tooling/BridgeProtocolCodec.cpp:170-441` |
| 229 | `src/application/generation/core/RequestAdapters.cpp:789-1017` |

## 6. 更新

```bash
python3 tools/architecture_audit.py --selftest
python3 tools/architecture_audit.py \
  --write-baseline doc/adr/audits/audit-baseline.json \
  --write-markdown doc/adr/audits/architecture-baseline.md
```

回归检查：

```bash
python3 tools/architecture_audit.py --baseline doc/adr/audits/audit-baseline.json
```
