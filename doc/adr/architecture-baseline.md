# ADR / architecture-baseline —— 架构审计数字基线

> **本文件是 RFC-001 及所有派生文档中架构审计数字的唯一真值源（single source of truth）。**
> 其它文档**不得**内联硬编码这些数字，只能引用本文的基线编号（`BL-*`）。
> 任何数字变化必须通过重跑 `tools/architecture_audit.py` 并更新本文件产生，禁止手工估算或笔误式沿用。

## 0. 快照元数据

| 项 | 值 |
|---|---|
| 生成时间（UTC） | `2026-08-07T18:36:35Z` |
| git commit | `1630e08` |
| 工作区状态 | **有 9 个未提交变更**（快照可能不可复现，提交后应重生成） |
| 审计工具 | `tools/architecture_audit.py` v2.0 |
| 依赖闭包模式 | `g++ -MM`（编译期真实闭包，非文件名匹配） |
| 编译器 | `g++ (Debian 12.2.0-14+deb12u1) 12.2.0` |
| 机器可读基线 | `doc/adr/audit-baseline.json` |
| 复现命令 | `python3 tools/architecture_audit.py --selftest && python3 tools/architecture_audit.py --baseline doc/adr/audit-baseline.json` |

### 0.1 可信度前置条件

本快照写入前 `--selftest` **已通过**：五个「已知有测试」的反例头文件中，`sessionManager/*` 全部被识别为已覆盖（断言数 > 0）。
自检失败时基线**不得**写入 —— 这是 v1.9 → v2.0 的教训：R2 曾用 basename 子串匹配，产出的数字不可信。

`accountManager/accountManager.h` 在自检中显示 `asserts=0` 属**预期**：它确实零覆盖，正是 R2 要抓的对象，不是工具缺陷。

---

## 1. 基线编号总表

| 编号 | 指标 | 值 | 门禁方向 |
|---|---|---|---|
| **BL-R1** | 同名竞争函数数 | **4** | 只减不增 |
| **BL-R2** | 高扇入零断言头文件数（fanin ≥ 2） | **22** | 只减不增 |
| **BL-R2-A** | 其中 A 类：已链接进测试二进制但零断言 | **5** | 优先清零 |
| **BL-R2-B** | 其中 B 类：未链接进测试二进制 | **17** | 逐条评估 |
| **BL-R2-H** | 其中 `impl_lines=0` 纯头文件组件（可能误报） | **3** | 先核实规则适用性 |
| **BL-R3** | 单函数 > 200 行的函数数 | **12** | 只减不增 |
| **BL-R3-L** | 上述函数总行数 | **4726** | 只减不增 |
| **BL-R3-F** | 涉及文件数 | **8** | — |
| **BL-SRC** | `src/` 生产源文件总数（排除 `test/`） | **64** | — |
| **BL-LINK** | 测试 target 直接编译的生产源文件数 | **32** | 只增不减 |
| **BL-UNLINK** | 未进入测试 target 的生产源文件数 | **32** | 只减不增 |
| **BL-TF** | 测试源文件数（`TEST_SOURCES`，含 `test_main.cc` 与 stub） | **26** | 只增不减 |
| **BL-TC** | `DROGON_TEST` 用例总数 | **174** | 只增不减 |
| **BL-TA** | `CHECK`/`REQUIRE` 断言总数 | **720** | 只增不减 |

> **BL-R2 的历史修正**：v1.9 记为 2，v2.0 首次重测记为 23，本次复测为 **22**。
> 23 → 22 的差值来自 `BackgroundTaskQueue.h` 因新增 `test_background_task_queue_shutdown.cpp` 而脱离零断言，
> **不是**口径再次变动。已废止的 v1.9 基线保留为 `audit-baseline.INVALID-r2-bug.json.bak` 以留痕。

---

## 2. R1 明细 —— 同名竞争（BL-R1 = 4）

**危害**：组件导出的自由函数与 `GenerationService::` 成员同名时，类作用域内的非限定调用永远命中成员版，
导致针对自由函数写的测试**空转**——用例全绿但生产路径根本没被执行。

| # | 函数名 | 声明位置 |
|---|---|---|
| 1 | `applyStrictClientRules` | `src/sessionManager/tooling/StrictClientRules.h` |
| 2 | `generateForcedToolCall` | `src/sessionManager/tooling/ForcedToolCallGenerator.h` |
| 3 | `normalizeToolCallArguments` | `src/sessionManager/tooling/ToolCallNormalizer.h` |
| 4 | `transformRequestForToolBridge` | `src/sessionManager/tooling/ToolDefinitionEncoder.h` |

---

## 3. R2 明细 —— 高扇入零断言（BL-R2 = 22）

**判定口径**：生产代码 `#include` 该头文件的次数 ≥ 2，且该头文件**未出现在任何测试二进制的编译期依赖闭包**中。
扇入按真实 `#include` 行解析，闭包由 `g++ -MM` 计算 —— v1.9 的 basename 子串匹配已废止。

### 3.1 A 类：已链接进测试二进制，但零断言（BL-R2-A = 5）

> **优先清空 A 类**：`.cpp` 已在 `PROJECT_SOURCES` 中，加测试不牵动链接闭包，每条边际成本相同且可预测。

| # | 扇入 | 实现行数 | 头文件 |
|---|---|---|---|
| 1 | 7 | 115 | `src/apiManager/ApiManager.h` |
| 2 | 3 | 490 | `src/dbManager/session/SessionDbManager.h` |
| 3 | 2 | 206 | `src/sessionManager/core/SessionCodec.h` |
| 4 | 2 | 28 | `src/apiManager/ApiFactory.h` |
| 5 | 2 | 22 | `src/sessionManager/continuity/TextExtractor.h` |

### 3.2 B 类：未链接进测试二进制（BL-R2-B = 17）

> B 类每条都要先改 `src/test/CMakeLists.txt` 引入源文件，可能牵出新的链接依赖，**不可批量估算工期**。

| # | 扇入 | 实现行数 | 头文件 |
|---|---|---|---|
| 1 | 5 | 175 | `src/channelManager/channelManager.h` |
| 2 | 5 | 73 | `src/retoolWorkspace/RetoolWorkspaceManager.h` |
| 3 | 4 | 2671 | `src/accountManager/accountManager.h` |
| 4 | 4 | 0 | `src/sessionManager/tooling/ToolDefinitionResolver.h` |
| 5 | 3 | 1441 | `src/apipoint/chaynsapi/chaynsapi.h` |
| 6 | 3 | 383 | `src/dbManager/chaynsThread/chaynsThreadDbManager.h` |
| 7 | 3 | 162 | `src/dbManager/account/accountBackupDbManager.h` |
| 8 | 3 | 87 | `src/sessionManager/core/ClientOutputSanitizer.h` |
| 9 | 3 | 30 | `src/sessionManager/tooling/ToolDefinitionEncoder.h` |
| 10 | 3 | 0 | `src/apipoint/ProviderResult.h` |
| 11 | 3 | 0 | `src/apiManager/Apicomn.h` |
| 12 | 2 | 532 | `src/sessionManager/core/GenerationService.h` |
| 13 | 2 | 452 | `src/dbManager/retoolWorkspace/RetoolWorkspaceDbManager.h` |
| 14 | 2 | 384 | `src/dbManager/channel/channelDbManager.h` |
| 15 | 2 | 181 | `src/retoolWorkspace/RetoolWorkspaceService.h` |
| 16 | 2 | 142 | `src/dbManager/config/ConfigDbManager.h` |
| 17 | 2 | 61 | `src/managedAccount/service/ManagedAccountService.h` |

### 3.3 待核实：`impl_lines = 0` 的纯头文件组件（BL-R2-H = 3）

这些条目没有对应 `.cpp`，需先判定「是否存在可断言的行为」，再决定是否属于 R2 的合理治理对象 —— **有可能是规则误报**。

- `src/sessionManager/tooling/ToolDefinitionResolver.h`（扇入 4）
- `src/apipoint/ProviderResult.h`（扇入 3）
- `src/apiManager/Apicomn.h`（扇入 3）

---

## 4. R3 明细 —— 单函数超长（BL-R3 = 12 / BL-R3-L = 4726 行 / BL-R3-F = 8 个文件）

**阈值**：单函数 > 200 行。**文件拆分无法解决单函数过长**，必须先做函数内拆分。

| # | 行数 | 文件 | 行号区间 |
|---|---|---|---|
| 1 | **865** | `src/apipoint/chaynsapi/chaynsapi.cpp` | 283–1147 |
| 2 | **555** | `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp` | 1660–2214 |
| 3 | **503** | `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp` | 515–1017 |
| 4 | **459** | `src/apipoint/retoolapi/retoolapi.cpp` | 847–1305 |
| 5 | **375** | `src/sessionManager/tooling/XmlTagToolCallCodec.cpp` | 678–1052 |
| 6 | **359** | `src/accountManager/accountManager.cpp` | 1807–2165 |
| 7 | **334** | `src/sessionManager/tooling/XmlTagToolCallCodec.cpp` | 344–677 |
| 8 | **283** | `src/sessionManager/core/GenerationService.cpp` | 142–424 |
| 9 | **272** | `src/sessionManager/tooling/BridgeProtocolCodec.cpp` | 170–441 |
| 10 | **258** | `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp` | 1385–1642 |
| 11 | **234** | `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp` | 1151–1384 |
| 12 | **229** | `src/sessionManager/core/RequestAdapters.cpp` | 795–1023 |

| — | **合计 4726** | 涉及 8 个文件 | — |

**关键观察**：全库最大单函数（865 行）不在通常被点名的 god 文件，而在 `src/apipoint/chaynsapi/chaynsapi.cpp`。
体量不等于风险 —— 判定依据必须是**调用图 + 同名竞争定义**，而非单纯行数。

---

## 5. 测试链接覆盖（BL-SRC / BL-LINK / BL-UNLINK）

真值来源为 `src/test/CMakeLists.txt` 的 `PROJECT_SOURCES` —— 这是测试二进制**实际链接**的文件集合。
已核实 `PROJECT_SOURCES` 中**无悬空引用**（列出的文件全部存在）。

- **BL-SRC = 64**，**BL-LINK = 32**，**BL-UNLINK = 32**（覆盖率 50.0%）

### 5.1 未进入测试 target 的生产源文件，按目录聚合

| 目录 | 未覆盖文件数 |
|---|---|
| `controllers` | 7 |
| `sessionManager/core` | 3 |
| `apipoint/chaynsapi` | 2 |
| `dbManager/account` | 2 |
| `managedAccount/backends` | 2 |
| `retoolWorkspace` | 2 |
| `<root>` | 1 |
| `accountManager` | 1 |
| `apipoint/nexosapi` | 1 |
| `apipoint/openai` | 1 |
| `apipoint/retoolapi` | 1 |
| `channelManager` | 1 |
| `dbManager/channel` | 1 |
| `dbManager/chaynsThread` | 1 |
| `dbManager/config` | 1 |
| `dbManager/metrics` | 1 |
| `dbManager/retoolWorkspace` | 1 |
| `managedAccount/service` | 1 |
| `sessionManager/tooling` | 1 |
| `tools/accountlogin` | 1 |

> 最大缺口在 `controllers/`（7 个）与 `sessionManager/core/`（3 个）。
> `controllers/` 是 HTTP 入口层，`sessionManager/core/` 是重构改动最集中的领域层 —— 两者叠加意味着**端到端主链路目前没有编译期可验证的安全网**。

<details>
<summary>完整清单（32 个文件）</summary>

- `src/accountManager/accountManager.cpp`
- `src/apipoint/chaynsapi/chaynsThreadReaper.cpp`
- `src/apipoint/chaynsapi/chaynsapi.cpp`
- `src/apipoint/nexosapi/nexosapi.cpp`
- `src/apipoint/openai/OpenAiProvider.cpp`
- `src/apipoint/retoolapi/retoolapi.cpp`
- `src/channelManager/channelManager.cpp`
- `src/controllers/AccountController.cc`
- `src/controllers/AiApiController.cc`
- `src/controllers/ChannelController.cc`
- `src/controllers/HealthController.cc`
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
- `src/managedAccount/backends/ClassicProviderAccountBackend.cpp`
- `src/managedAccount/backends/RetoolWorkspaceBackend.cpp`
- `src/managedAccount/service/ManagedAccountService.cpp`
- `src/retoolWorkspace/RetoolWorkspaceManager.cpp`
- `src/retoolWorkspace/RetoolWorkspaceService.cpp`
- `src/sessionManager/core/ClientOutputSanitizer.cpp`
- `src/sessionManager/core/GenerationService.cpp`
- `src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp`
- `src/sessionManager/tooling/ToolDefinitionEncoder.cpp`
- `src/tools/accountlogin/login_client.cpp`

</details>

---

## 6. 门禁契约

```bash
# 1) 先验证审计工具自身可信
python3 tools/architecture_audit.py --selftest      # 退出码必须为 0

# 2) 再做基线回归检查
python3 tools/architecture_audit.py --baseline doc/adr/audit-baseline.json
# 退出码 0 = 通过；1 = R1/R2/R3 任一超过基线，判定为架构回归
```

**规则**：

1. `--selftest` 失败时，本文件所有数字**立即失效**，不得用于任何决策。
2. 基线只允许**下降**。要提高基线必须在本文件记录原因与决策人。
3. 每次基线变更需同步更新 `doc/adr/audit-baseline.json` 与本文第 0 节的元数据。
4. 「用例全绿」**不单独构成**阶段验收条件，必须附带「空转用例数 = 0」。

---

## 7. 引用方式

其它文档引用示例：

> 当前高扇入零断言组件共 **BL-R2** 个，其中 **BL-R2-A** 个已链接进测试二进制（详见 `doc/adr/architecture-baseline.md` §3.1）。

**禁止**写成「当前高扇入零断言组件共 22 个」—— 数字会随代码演进漂移，内联硬编码必然产生前后矛盾。

---

## 8. R2 分类法口径（A / H / B 三分）

> 本节由 RFC-001 §0-E-3（v2.2）外移而来。分类法是**审计口径定义**，
> 与本文第 3 节的 R2 明细表同源，放在一起才不会再次出现「口径与数字分居两处」的漂移。


### 推翻 v2.0 自己定的 A/B 二分法

v2.0 把当时的 23 条分为「A 已链接(5)」与「B 未链接(18)」，判据是 `.cpp` 是否在 `PROJECT_SOURCES`。
（此处 23/5/18 为**当时快照**，仅作历史叙述；当前值见 `architecture-baseline.md` §1。）
**该判据对纯头组件是错的** —— header-only 组件根本没有 `.cpp` 可链接，测试它只需 `#include`，**CMake 一行都不用改**。
把它们归入「须先改 CMake」的 B 类，等于凭空记了 4 笔不存在的成本。

### 修正后的三分法

| 类别 | 数量 | 判据 | CMake 成本 |
|---|---:|---|---|
| **A 已链接** | **BL-R2-A** | `.cpp` 已在 `PROJECT_SOURCES` | `TEST_SOURCES` 加一行 |
| **H 纯头（新增）** | **BL-R2-H** | `impl=0` 且头内有 inline 函数体 | **零** |
| **B 未链接** | **14** | 有 `.cpp` 但不在 `PROJECT_SOURCES` | 须改 CMake + 评估链接闭包 |
| 合计 | **BL-R2** | H + B 之和须等于 **BL-R2-B**（见 `architecture-baseline.md` §1 复算） | |

> **v2.5 对账**：原表另有「排除 1 条（无可断言行为）」并把合计记为 23。
> 该 1 条为 `BackgroundTaskQueue.h`，已因新增 `test_background_task_queue_shutdown.cpp` **脱离 R2**，不再出现在命中列表 —— 故该行删除、合计降为 **BL-R2**。
> 这是基线**真实下降 1 条**，不是重新分类。

### H 类逐条结论

> **v2.5 状态**：下表第三行 `BackgroundTaskQueue.h` 已因新增 `test_background_task_queue_shutdown.cpp` **脱离 R2**，不再计入 **BL-R2-H**（当前 H 类为前两条）。保留该行仅为留痕，**不得再据此排期**。

| 组件 | 头行数 | inline 函数体 | 结论 | 排期 |
|---|---:|---:|---|---|
| `tooling/ToolDefinitionResolver.h` | 176 | 11 | **真实缺口，价值最高**。`namespace toolcall` 下的自由函数，含 `visitToolDefinitionsImpl` 对嵌套 namespace 工具定义的**递归下降遍历**（`declaredType == "namespace"` 分支、`_aiapi_namespace` 元数据回读）。纯函数、无全局状态 | **H1 · 1 天** |
| `apipoint/ProviderResult.h` | 152 | 12 | **真实缺口，成本极低**。`isSuccess()` / `hasError()` / `isValid()` 三个判定式 + 5 个静态工厂。`fail()` 内 `err.httpStatusCode > 0 ? … : 500` 是唯一有真实分支的工厂 | **H2 · 0.5 天** |
| `utils/BackgroundTaskQueue.h` | 152 | 14 | **真实但不属本阶段**。函数内静态单例 `static BackgroundTaskQueue& instance()` + `std::vector<std::thread> workers_` 线程池。测它需控制线程调度与 `shutdown()` 时序，**与阶段 2「消灭单例」是同一件事**，此处单独补测会做两遍 | **不排期，转阶段 2** |

### 排除项：`Apicomn.h`（真误报）

全文 13 行：`struct session_st;` / `class APIinterface;` 两个前向声明，加一个只有单个枚举值的 `enum class ApiChannel`。
**无函数、无数据成员、无任何可断言行为** —— 为它写测试只能断言「枚举值等于自己」，正属 §0-E 定义的空转用例。

> **规则修正建议（仍未执行）**：R2 应增加排除条件「`impl=0` 且 inline 函数体数 = 0」，以自动排除 `Apicomn.h` 这类无可断言行为的前向声明头。
> **v2.5 状态更新**：脚本规则**依旧未改**（改规则必须单独 commit + 重跑 `--selftest`，混进文档提交会让防回归失去意义）。
> 但脚本口径已由 23 降为 **BL-R2** —— 原因是 `BackgroundTaskQueue.h` 新增单测后真实脱离 R2，**与规则修正无关**。
> 因此当前差异收敛为：**脚本口径 BL-R2 条，其中 1 条（`Apicomn.h`）为已知误报待规则修正后自动消除**。

### B 类 14 条分层（替代 v2.0 的扁平列表）

| 层 | 条数 | impl 行数 | 处置原则 |
|---|---:|---|---|
| **B1 小** | **8** | 30–181 | 可逐条立项，单条 0.5–1 天。扇入最高的 `channelManager.h`(5/175)、`RetoolWorkspaceManager.h`(5/73) 优先 |
| **B2 中** | **3** | 383–452 | 三条全是 `dbManager`（chaynsThread / retoolWorkspace / channel）。**须先定 DB 替身策略**，与 §0-E-2 的 P4 同一前置 |
| **B3 大** | **3** | 532–2601 | `accountManager.h`(2671) / `chaynsapi.h`(1441) / `GenerationService.h`(532)。**禁止估时**；前两条已在 §7 风险表单列，须先函数内拆分 |

> **B2 与 P4 合并前置**：`SessionDbManager`（P4，490 行）与 B2 三条面对同一个问题 —— 真实 DB + 异步队列。
> 一次性定出 DB 替身策略可同时解锁 4 条，比逐条摸索更省。**但该策略尚未设计，故这 4 条一律不估时。**

---


---

## 附录 C：统计口径固化（P5-7）

P5-5 的交叉校验一次报出三处「文档与实测不一致」，逐项定性后**三处全部是校验脚本的缺陷，文档数字无误**。
把踩过的坑写成明文口径，避免后续复测重复翻车。

| 指标 | 文档值 | 错误脚本得数 | 错因 | 正确口径 |
|---|---:|---:|---|---|
| 单例访问器 | **23** | 9 | 返回类型字符集写成 `[A-Za-z_:<>, ]*`，无法覆盖 `std::shared_ptr<X>` 形态 | `static [^;]*(instance｜getInstance)` + `(`，范围 `*.h` |
| `ResponseIndex` 调用点 | **44** | 35 | **漏了 `--include=*.cc`** —— `main.cc` 与 `AiApiController.cc` 共 9 处 | 三种扩展名齐上：`*.h *.cpp *.cc` |
| `catch(std::exception)` | **90** | 81 | ERE 模式下误用 BRE 转义（被当字面括号），且只扫 `.cpp` | `catch` + `(` + 可选 `const` + `std::exception`，范围 `*.cpp *.cc *.h` |

**三条口径规则（后续所有复测必须遵守）**：

1. **扩展名必须含 `.cc`**。本项目 `src/main.cc` 与 `src/controllers/AiApiController.cc` 用 `.cc`，
   只写 `--include=*.cpp` 会系统性漏掉**入口与控制器** —— 恰是扇入最高处，漏计必然低估。
2. **`grep -E` 下不得使用 BRE 转义**。转义括号与转义问号在 ERE 中是字面量，会静默少计而不报错，是最难发现的一类统计 bug。
3. **是否排除 `src/test/` 必须显式声明**。`catch` 的 81 / 90 差值中 test 目录贡献为 0 ——
   差值全部来自扩展名，**不要把扩展名问题误判成 test 过滤问题**。

> **附带发现**：`catch(...)` 任意类型共 **121** 处，其中 `catch(std::exception)` 90 处。
> 即另有约 31 处捕获其他类型或 `catch(...)`，ADR-05 §5.3 的「异常转 Error」边界改造**不能只盯那 90 处**。

> **与 §1 BL-R2 的区分**：附录 C 的「单例访问器 23」与 §1 的 `BL-R2=22` **不是同一指标** ——
> 前者数访问器声明，后者数零断言头文件。数值接近纯属巧合，切勿互相套用。
