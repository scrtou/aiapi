# RFC-001 派生文档 · 迁移执行计划（migration-plan）

> **本文件承接 RFC-001 §6「分阶段执行计划」全文，以及 §0-F「依赖环实测与阶段 0.7 解环」。**
> RFC-001 正文只保留阶段清单与工期汇总，任何阶段的**任务级细节以本文为准**。
> 数字引用一律指向 `architecture-baseline.md` 的 `BL-*` 编号，本文不内联硬编码审计数字。

| 关联文档 | 用途 |
|---|---|
| [`RFC-001-architecture-refactor.md`](./RFC-001-architecture-refactor.md) | 决策与目标（上游） |
| [`architecture-baseline.md`](./architecture-baseline.md) | 审计数字唯一真值源 |
| [`CHANGELOG.md`](./CHANGELOG.md) | 版本演进与自我纠错留痕 |

---

## 6. 分阶段执行计划

每阶段独立可发布；完成标志一律为「旧代码已删除」，而非「新代码已可用」。

### 阶段 0 · 安全网（1.5 周）

> **修订说明（v1.4，纠正 v1.3）**：v1.3 判定 `har/` 不可用是**错误结论**——
> 当时按「找 `/v1/chat/completions` 与 SSE」的思路检索，而本服务上游 chayns **不用 SSE，走轮询**，
> 因而误判。重新核查后，`har/` **可用且是当前唯一活跃上游的真实录制**。v1.3 的废止决定作废。

#### 0-A `har/` 可用性复核：可用（覆盖唯一活跃上游）

**前提**：目前实际使用的上游只有 **chayns**；`retool` 保留但不活跃；
`nexos` / `openai` 已决定在**阶段 0.5 删除**，因此不为其投入任何安全网成本。
因此安全网只需覆盖 chayns 一家即可守住真实流量，其余 Provider 降级处理。

`har/` 与 `chaynsapi.cpp` 端点比对：

| `chaynsapi.cpp` 端点 | har 中有对应录制 | 请求体 | 响应体 |
|----------------------|:----------------:|:------:|:------:|
| `POST /intercom-backend/v2/thread?forceCreate=true` | ✅ | ✅ 170 B | ✅ 1,083 B (201) |
| `POST /intercom-backend/v2/thread/{id}/message` | ✅ | ✅ 35 B | ✅ 236 B (201) |
| `GET  /intercom-backend/v2/thread/{id}/message`（轮询取回复） | ✅ | — | ✅ 815 B (200) |
| `DELETE /intercom-backend/v2/thread/member/delete` | ✅ | ✅ 61 B | ✅ 200 |
| `GET  /chayns-ai-chatbot/nativeModelChatbot`（模型清单） | ✅ | — | ✅ 52,260 B |
| `PATCH /intercom-backend/v2/thread/read` | ✅ | ✅ 52 B | ✅ 200 |

**代码使用的 6 类端点，har 全部命中，且带完整请求体与响应体。**
合计 20 个有 body 的 chayns 上游端点可直接落为 fixture。

**为何 v1.3 误判**：

1. chayns 上游**不是 SSE**。`chaynsPollingPolicy.h` 明确是轮询式：
   `kRequestPollingDeadline = 5min`，`pollingDelayForElapsed()` 做退避。
   流式效果由「POST 发消息 → 循环 GET `/message` 拉增量」实现，全程 `application/json`。
   按 `text/event-stream` 检索自然一条都找不到。
2. har 的 WebSocket 帧被 `_webSocketMessages` 字段承载，首轮统计未读取该字段。
   实际含 3 条 WS 连接、77 帧（`register` / `registered` / `ping` / `pong` 等），
   但代码侧 `src/` 内**无任何 websocket 引用**——WS 是前端推送通道，本服务不使用，与安全网无关。

#### 0-B 安全网方案（基于真实 har）

| 层 | 手段 | 覆盖对象 | 数据来源 |
|----|------|----------|----------|
| L1 **chayns 契约回放** | 从 har 抽真实请求/响应对 → 脱敏 → 喂解析器 → 逐字节快照 | `chaynsapi.cpp` (1,440 行) | **har，真实录制** |
| L2 轮询时序测试 | 假时钟驱动 `pollingDelayForElapsed` 与 5 min 截止 | `chaynsPollingPolicy` + 轮询主循环 | har 中多轮 GET 的真实时间戳 |
| L3 特性化测试 | 照原样锁定现状（不修 bug） | `accountManager`、`EmitAndToolBridge` | 手写 |
| L4 冒烟兜底 | 起服务 + 假上游 stub 跑完整链路 | 路由、鉴权、错误码 | 手写 |

> **retool / nexos / genspark 三个 Provider**：既非活跃上游，又无录制。
> 不为其编写契约测试，仅保证**编译通过 + 构造析构不崩**。
> 阶段 3 归一时若行为漂移，风险由「该上游本就未启用」吸收。
> 若日后重新启用，须先补 fixture 再上线——此约束写入 §8 工程纪律。

#### 0-C 任务清单

| 任务 | 产出 | 工期 |
|------|------|-----:|
| har 抽取脚本 | `tools/har2fixture.py`：按端点切分，token / personId / siteId 归一为固定假值 | 1 天 |
| 快照框架 | `loadFixture()` / `assertSnapshot()` | 0.5 天 |
| L1：chayns 6 类端点契约回放 | 6 组请求构造 + 6 组响应解析快照 | 2 天 |
| L2：轮询时序测试 | 退避曲线、截止、空响应、乱序到达 | 1.5 天 |
| L3：`accountManager` 特性化 | 配额、轮换、失效标记 | 2 天 |
| L3：`EmitAndToolBridge` 特性化 | emit 时序、工具调用装配、中断恢复 | 2 天 |
| 记录基线指标 | 构建时长、二进制体积、P99 延迟 | 0.5 天 |
| ~~修复 `chatSession` 双实例隐患~~ → **删除死成员变量** | v1.6 已澄清为误判（见 P2b），降为 10 分钟的清理，并入批次 2-a | 0.1 天 |
| **测试有效性审计**（v1.7 新增，v1.8 降工期） | `tools/architecture_audit.py`；实现 **R1/R2/R3 三条规则**（见 0-E-1），输出基线快照并在 CI 里防回归 | 0.5 天 |
| **新增（v1.8）：`transformRequestForToolBridge` 特性化** | 554 行 / 每请求必经 / 零覆盖；锁定协议格式决策、Codex systemPrompt 清空、definition_mode 开关、触发标记注入 | 1.5 天 |
| ~~修复标准漂移（P9）~~ | **已完成**，commit `efb4003`，见 §0.4b | — |

#### 0-D 门禁

- chayns 6 类端点契约回放全绿，`ctest` 可离线重复执行（无网络、无真实时间依赖）
- `accountManager` 与 `EmitAndToolBridge` 特性化测试全绿
- 模块覆盖率（§0.5 口径）41.0% → **≥ 60%**
- `chaynsapi.cpp`、`accountManager.cpp`、`EmitAndToolBridge.cpp` 三个文件脱离零覆盖
  （retool 允许维持零覆盖；nexos / openai 因阶段 0.5 删除而不计入分母）
- `-std=` 在主程序与测试中一致（**已满足**）

> **凭据处理**：har 内的 JWT / `personId` / `siteId` 由维护者在数据库侧轮换，
> 不作为本 RFC 的阻塞项。`har2fixture.py` 仍统一把这些字段替换为**固定假值**，
> 但目的是**测试可复现性**（避免 fixture 因真实 ID 变动而漂移），不是安全兜底。

---

#### 0-E （v1.7 新增）安全网的前置条件：先验收现有测试是否真在保护生产路径

> **触发原因**：阶段 4 踏点时实测发现，至少 10 个已有用例测的是生产不走的另一份实现（详见 §6 阶段 4 的 4-0）。

原方案把「全部用例全绿」当作每个阶段的默认门禁。但用例只能证明「它 include 的那份代码没坏」，
不能证明「生产走的那份代码没坏」—— 当同一个函数名存在两份实现时，两者可以完全脱钩。

**审计方法**（可自动化）：

1. 对每个 `src/test/*.cpp`，提取其 include 的业务头文件与被测函数名
2. 在非测试代码中搜同名函数的**定义**，统计定义份数
3. 若 > 1 份，检查生产调用点的**名字查找结果**（是否带命名空间限定、是否处于类作用域内）
4. 产出三类结果：**有效** / **空转**（测的不是生产那份）/ **存疑**

**门禁变更**：自 v1.7 起，「用例全绿」不再单独构成阶段验收条件，必须附带**空转用例数 = 0**。
**v1.8 审计已先行手工跑完，结果如下**（脚本化仅用于防回归）：

| 函数 | 成员版行数 | 组件版行数 | 成员版性质 | 测试实际测的 | 判定 |
|------|------:|------:|------|------|------|
| `applyStrictClientRules` | **6** | 200 | **转发壳** | 组件版 | ✅ 健康 |
| `generateForcedToolCall` | 234 | 51 | 完整实现 | 组件版 | ❌ 空转 |
| `normalizeToolCallArguments` | 258 | 41 | 完整实现 | 组件版 | ❌ 空转 |
| `transformRequestForToolBridge` | **554** | 29 | 完整实现 | **无测试** | ⚠️ 裸奔 |

**分母口径**：`DROGON_TEST` 实测数记为 **BL-TC**（分布于 **BL-TF** 个文件），见 `architecture-baseline.md` §1；历史版本曾写 160，为笔误沿用，已废止。

#### 0-E-1 三条审计规则（v1.9 定稿，全阶段共用同一把尺子）

本节是后续所有阶段的**唯一判定依据**。任何「某文件很危险」的断言，必须能映射到下列三条之一，否则不得写入方案。

| 规则 | 定义 | 判定方法 | 为何必要 |
|---|------|------|------|
| **R1 同名竞争** | 组件导出自由函数与 `GenerationService::` 成员同名 | 扫描 `tooling/`+`actionProtocol/` 头文件函数名，交叉比对 `core/` 成员定义 | 类作用域内非限定调用永远命中成员版 → **测试空转** |
| **R2 高扇入零测试** | 生产 `#include` 数 ≥ 2 且**未进入任何测试二进制的依赖闭包** | **v2.0 改**：以 `src/test/CMakeLists.txt` 的 `TEST_SOURCES`/`PROJECT_SOURCES` 为真值，用 `g++ -MM` 求编译期真实依赖闭包；扇入按真实 `#include` 行解析（**v1.9 的 `grep -rl` basename 子串匹配已废止**） | 被多处依赖却无任何验证 → **改动无安全网** |
| **R3 函数级行数** | 单函数 > 200 行 | 按顶层函数定义行切分相邻边界 | **文件拆分无法解决单函数过长**，必须先做函数内拆分 |

**配套硬规则**：所有行数必须来自 `wc -l` 或脚本计算，**不得目测估算**。
（v1.8 曾将 `ToolDefinitionEncoder` 误记为 26 行，实为 **29** 行；`transformRequestForToolBridge` 估为 ~550，实为 **554**。）

##### R1 实测：4 个命中，已收敛

`applyStrictClientRules`（转发壳，健康）、`generateForcedToolCall`、`normalizeToolCallArguments`、`transformRequestForToolBridge`。
本轮独立重扫结果与 v1.8 完全一致 → **无遗漏**。

##### R2 实测：**BL-R2** 个命中

> 完整明细（A 类 / B 类 / `impl=0` 待核实项）见 `architecture-baseline.md` §3，本节只保留处置判断。
> v1.9 曾记为 2，系 basename 子串匹配的口径错误，**不是漏扫**；v2.0 起改用 `g++ -MM` 编译期闭包。

> **v1.9 写的 2 个是错的。** 当时用「头文件 basename 子串匹配 `src/test/`」判断是否被测：
> 既会把「测试文件名里恰好出现该词」误判为已覆盖，也会漏掉「经传递包含间接进入测试二进制」的情况。
> v2.0 的 `tools/architecture_audit.py` 改用 **`g++ -MM` 编译期真实依赖闭包**，
> 并以 `src/test/CMakeLists.txt` 的 `TEST_SOURCES` / `PROJECT_SOURCES` 为真值来源
> —— 这是测试二进制**实际链接**的文件集合，比任何字符串猜测都可靠。
> 同时修正测试信号识别（`DROGON_TEST` / `CHECK` / `REQUIRE`，此前误按 gtest 假定）。
> 基线快照落盘 `doc/adr/audit-baseline.json`；错误版本留档 `audit-baseline.INVALID-r2-bug.json.bak`。

`BridgeHelpers`（188 行 / 扇入 5）已于本轮补测并出榜（`src/test/test_bridge_helpers.cpp`）。

**完整榜单**（`impl` = 对应 `.cpp` 行数，`链接` = 该 `.cpp` 是否已在 `PROJECT_SOURCES` 中）：

| 扇入 | impl | 链接 | 头文件 | 类 |
|---:|---:|:--:|------|:--:|
| 7 | 115 | ✅ | `apiManager/ApiManager.h` | **A** |
| 5 | 175 | ❌ | `channelManager/channelManager.h` | B |
| 5 | 73 | ❌ | `retoolWorkspace/RetoolWorkspaceManager.h` | B |
| 4 | 2601 | ❌ | `accountManager/accountManager.h` | B |
| 4 | 0 | ❌ | `sessionManager/tooling/ToolDefinitionResolver.h` | B（纯头） |
| 4 | 0 | ❌ | `utils/BackgroundTaskQueue.h` | B（纯头） |
| 3 | 1441 | ❌ | `apipoint/chaynsapi/chaynsapi.h` | B |
| 3 | 490 | ✅ | `dbManager/session/SessionDbManager.h` | **A** |
| 3 | 383 | ❌ | `dbManager/chaynsThread/chaynsThreadDbManager.h` | B |
| 3 | 162 | ❌ | `dbManager/account/accountBackupDbManager.h` | B |
| 3 | 87 | ❌ | `sessionManager/core/ClientOutputSanitizer.h` | B |
| 3 | 30 | ❌ | `sessionManager/tooling/ToolDefinitionEncoder.h` | B（亦命中 R1） |
| 3 | 0 | ❌ | `apipoint/ProviderResult.h` | B（纯头） |
| 3 | 0 | ❌ | `apiManager/Apicomn.h` | B（纯头） |
| 2 | 532 | ❌ | `sessionManager/core/GenerationService.h` | B |
| 2 | 452 | ❌ | `dbManager/retoolWorkspace/RetoolWorkspaceDbManager.h` | B |
| 2 | 384 | ❌ | `dbManager/channel/channelDbManager.h` | B |
| 2 | 206 | ✅ | `sessionManager/core/SessionCodec.h` | **A** |
| 2 | 181 | ❌ | `retoolWorkspace/RetoolWorkspaceService.h` | B |
| 2 | 142 | ❌ | `dbManager/config/ConfigDbManager.h` | B |
| 2 | 61 | ❌ | `managedAccount/service/ManagedAccountService.h` | B |
| 2 | 28 | ✅ | `apiManager/ApiFactory.h` | **A** |
| 2 | 22 | ✅ | `sessionManager/continuity/TextExtractor.h` | **A** |

**A/B 分类是本节的核心结论**，处置成本相差一个数量级：

| 类别 | 数量 | 特征 | 处置成本 |
|---|---:|---|---|
| **A：已链接** | **BL-R2-A** | `.cpp` 已在 `PROJECT_SOURCES`，只是没人写用例 | 新增 `test_*.cpp` + `TEST_SOURCES` 加一行，**零构建风险** |
| **B：未链接** | **BL-R2-B** | `.cpp` 不在 `PROJECT_SOURCES`，或为纯头文件组件 | 须先改 CMake 引入源文件，可能牵出新链接依赖，**逐条评估** |

> **排序原则：先清空 A 类，再动 B 类。** A 类每条边际成本相同且可预测；
> B 类每条都要单独评估链接闭包，**不可批量估算工期**。
> 其中 **BL-R2-H** 条为 `impl=0` 的纯头文件组件（清单见 `architecture-baseline.md` §3.3），
> 需先判定「是否存在可断言行为」再决定是否属于 R2 的合理治理对象 —— 有可能是**规则误报**，待逐条核实。
> **v2.5 更新**：`BackgroundTaskQueue.h` 已因新增 `test_background_task_queue_shutdown.cpp` 脱离 R2，不再计入。

##### R3 实测：**BL-R3** 个函数 / **BL-R3-F** 个文件 / **BL-R3-L** 行

> 权威明细（含精确行号区间）见 `architecture-baseline.md` §4。下表仅补充**函数名归属**这一基线未记录的语义信息，行号可能滞后，**冲突时以基线为准**。

| 行数 | 文件 | 位置 |
|---:|------|------|
| **865** | `chaynsapi.cpp` | 283–1147 |
| **555** | god 文件 `transformRequestForToolBridge` | 1660–2214 |
| **503** | god 文件 `emitResultEvents` | 515–1017 |
| 459 | `retoolapi.cpp` | 847–1305 |
| 375 | `XmlTagToolCallCodec.cpp` | 678–1052 |
| 359 | `accountManager.cpp` | 1807–2165 |
| 334 | `XmlTagToolCallCodec.cpp` | 344–677 |
| 283 | `GenerationService.cpp` `executeGuardedWithSession` | 142–424 |
| 272 | `BridgeProtocolCodec.cpp` `adaptCompiled` | 170–441 |
| 258 | god 文件 `normalizeToolCallArguments` | 1385–1642 |
| 234 | god 文件 `generateForcedToolCall` | 1151–1384 |
| 229 | `RequestAdapters.cpp` | 795–1023 |

**全库最大单函数不在 god 文件，而在 `chaynsapi.cpp`（865 行）** —— 且该文件已知**零覆盖**。


`ActionProtocolAdapter.cpp` 仅 75 行但功能完整且生产在用；判定依据必须是**调用图 + 同名竞争定义**，而非体量。


---

#### 0-E-2 R2 首批处置方案（v2.0 新增，A 类优先）

> 本节是 R2 从 **BL-R2** 条开始下降的**第一批执行单**。目标选取只依据 0-E-1 的 R2 定义，不引入新尺子。

##### A 类（**BL-R2-A** 条）的处置排序

| 序 | 目标 | 扇入 | impl | 选它的理由 | 阻塞项 |
|---|---|---:|---:|---|---|
| **P0** | `SessionCodec.h` | 2 | 206 | 纯函数、依赖闭包仅 5 个头、往返契约可完整验证 | **无** |
| **P1** | `TextExtractor.h` | 2 | 22 | 全榜体量最小、零依赖，边际成本最低 | **无** |
| **P2** | `ApiManager.h` | **7（全榜最高）** | 115 | 补测会直接暴露 P13 的 4 个缺陷，价值最高 | `provider::ProviderResult` 可否默认构造**未探明** |
| P3 | `ApiFactory.h` | 2 | 28 | 与 P2 同模块，可复用其替身 | 依赖 P2 的替身结论 |
| P4 | `SessionDbManager.h` | 3 | 490 | 已链接，但涉及真实 DB 与异步队列 | 需先确认可否用内存库或注入替身，**非纯函数** |

**P0/P1 的构建改动面仅为 `TEST_SOURCES` 加一行** —— 二者 `.cpp` 均已在 `PROJECT_SOURCES` 中，`OpenSSL::Crypto` 已链接，`Session.h` 中 SHA256 仅出现于注释与函数声明、无内联实现，**不产生新的符号依赖**。

##### 已排除项及理由（避免后续重复讨论）

| 不做 | 理由 |
|---|---|
| 测 `ApiManager::init()` | 依赖 `ApiFactory` 全局注册表，而各 provider 静态注册器未链入测试二进制 → 遍历近乎空的 map，**无信息量**；且写 `getInstance()` 单例，会跨用例污染 |
| 修 `TextExtractor` 两条路径的空串过滤不对称 | `continuityTexts` 分支不过滤空串、`messages` 分支过滤。注释显示与「保留零宽字符」有关，**可能是刻意设计** —— 先写断言钉住现状，不擅自改 |
| 动 `Session.h` 顶部 `static const int` 常量与中段 `#include` | 确为代码风味问题，但与 R2 无关，**不夹带** |
| 一次性清空 R2 | 剩余 **BL-R2-B** 条为 B 类，须先改 CMake，属另一类工作 |

---

##### P0 · `SessionCodec` 详细方案

**字段矩阵**（`session_st` 共 **32** 个字段；encode / decode / 结构体声明三方已逐字段机械核对一致）：

> **v2.1 实证**：`SessionCodec.cpp` 中四段对应的局部变量为 `req`(10) / `resp`(4) / `st`(11) / `pv`(7)，
> 另有 `root`(5) 为顶层包装、`one`(5) 为 `images` 元素 —— 与 `Session.h` 结构体声明**逐段吻合**，
> 10+4+11+7 = **32** 可复算。哨兵用例 2 的四个数字即取自这四个变量。

| 段 | 字段数 | 非默认值字段 | 无类型守卫的 `Json::Value` 字段 |
|---|---:|---|---|
| `RequestData` | 10 | `parallelToolCalls = true` | `tools`、`toolsRaw` |
| `ResponseData` | 4 | — | `message`、`apiData` |
| `SessionState` | 11 | `apiType = ChatCompletions` | — |
| `ProviderContext` | 7 | `supportsToolCalls = true`、`messageContext = arrayValue` | `clientInfo` |
| **合计** | **32** | **4** | **5** |

嵌套层：`images` 为 `vector<ImageInfo>`，每元素 **5** 个子字段（`base64Data` / `mediaType` / `uploadedUrl` / `width` / `height`），三方一致。

> **v2.1 实证补注**：`ImageInfo` 定义于 `src/sessionManager/contracts/GenerationRequest.h:110`（**不在 `Session.h`**），
> 按结构体边界机械计数确认为 5 字段。此前该数字未标出处，不符合 §0-E-1「行数/字段数必须可复算」的硬规则。

**阶段 1 —— 只加测试，不碰生产代码**（10 条用例；每条须写明抓什么，不得只写「覆盖」）：

| # | 用例 | 能抓到什么 |
|---|---|---|
| 1 | 32 字段全非默认值往返恒等 | 新增字段忘改 codec → 红。**核心资产** |
| 2 | 段 key 数哨兵 `10/4/11/7` | 只改 encode 未改 decode 的半边错误 —— 用例 1 抓不到（往返比对时两边都是默认值） |
| 3 | 空快照 `{}` 的 4 个非默认值 | 默认值被误改成 `false` / `null` |
| 4 | 脏类型静默降级 | `contextLength` 给 `"5"`、`createdAt` 给字符串 → 回落默认。往返测试**永远测不到** |
| 5 | 无守卫字段类型穿透 | `clientInfo` 给数组、`tools` 给字符串 → 断言**原样穿透**。为阶段 2 改动建立基线 |
| 6 | payload 非 object | array / null / 标量 / 空串 → 全默认，固定短路语义 |
| 7 | `apiType` 双向 + 越界 | **枚举已确认仅 2 值**（`ChatCompletions` / `Responses`），三元映射完备，用例据此收窄不留冗余分支 |
| 8 | `bridgeFormat` 双向 + 越界 | 三值枚举，三条都打 + `default` 回落 `Unset` |
| 9 | `images` 空 / 多元素顺序 / 非数组 | 顺序是唯一的数组语义，须逐元素比对 |
| 10 | `messageContext` 非数组纠正 | 守住 `addMessageToContext` 的 `append` 不炸 |

**阶段 2 —— 测试绿灯后才改生产代码**（3 处，均为读代码发现的真实问题）：

| 改动 | 性质 | 为何必须后置于测试 |
|---|---|---|
| `v` 字段：`decodeSession` 读取 + 未知版本告警 | 死字段复活 | 全仓 `["v"]` **仅 1 处引用**（`SessionCodec.cpp:75` 写入），**读取方为零**。所谓「向前兼容」实由逐字段默认值实现，与版本号无关。改 decode 入口需往返测试兜底 |
| 消除 `apiType` **三份**映射 | 去重 | `SessionCodec.cpp:41` `apiTypeToInt` / `Session.h` `isResponseApi()` / `Session.cpp:43` `s.isResponseApi() ? 1 : 0`。第三处把布尔判断与整数编码混写，改 codec 会使 DB 列静默不同步。方案：`apiTypeToInt` 提出匿名 namespace 公开，`Session.cpp:43` 改为调用它 |
| `clientInfo` 补 `isObject()` 守卫 | 对称化 | 与紧邻的 `messageContext`（有 `isArray()` 兜底且写了注释）不对称。此改动会**改变用例 5 的预期**，故用例 5 必须先存在 |

**阶段 3 —— 变异验真（不可跳过；本轮由 1 条增至 4 条）**：

| 变异 | 期望 | 不红说明什么 |
|---|---|---|
| encode 注释掉 `req["toolChoice"]` | 用例 1 + 2 双红 | **构造体该字段用了默认值 → 用例 1 是假的，必须重写** |
| `getBool` 默认 `true` 改 `false` | 用例 3 红 | 默认值断言未绑到实处 |
| `bridgeFormatFromInt` 的 `default` 改返回 `Json` | 用例 8 红 | 越界分支未测到 |
| 去掉 `messageContext` 的 `isArray()` 三元 | 用例 10 红 | 类型纠正未测到 |

> 上一轮只做 1 条变异。用例间存在覆盖重叠，**单条变异不足以证明每条断言独立有效**。

---

##### P1 · `TextExtractor` 详细方案

三分支全覆盖；全空输入返回空 vector；`continuityTexts` 优先级压过 `messages`（即使 `messages` 非空也不读）；`currentInput` 追加于末尾且顺序在 `messages` 之后。
外加一条断言钉住上文所述的**空串过滤不对称**：若为有意设计，断言即文档；若为疏漏，将来修复时会先撞上它。

---

##### P2 · `ApiManager` 详细方案（定性已变：从「补测试」变为「测试 + 修 P13」）

**替身可行性**：`addApiInfo` 只访问 `APIinterface::ModelInfoMap` **public 数据成员**，不调虚函数。替身需实现 **8 个纯虚函数**（`generate` / `checkAlivableTokens` / `checkModels` / `getModels` / `init` / `afterResponseProcess` / `eraseChatinfoMap` / `transferThreadContext`）。`ApiManager()` 构造函数为 public，测试可建**局部实例**绕开 `getInstance()`，避免跨用例污染。

**唯一未探明项**：`generate` 返回的 `provider::ProviderResult` 能否零成本默认构造。`ProviderResult.h` 本身就在 R2 榜上且为**未链接**状态（扇入 3 / impl 0）—— 这决定 P2 究竟是 A 类还是会退化为 B 类。

**执行顺序（不可颠倒）**：

1. 探明 `ProviderResult` 可构造性 → 定替身成本
2. 写替身，测**当前行为**（含把「空队列 `top()` 会崩」记录为已知缺陷，此步**不修**）
3. 修 P13 四项，测试语义由「记录现状」转为「验证修复」

> 第 2 步不可跳过：直接改代码就没有基线证明「改对了」。

---

##### 本批预期收益

| 指标 | 当前 | P0 后 | P0+P1 后 |
|---|---:|---:|---:|
| R2 条目 | 23 | 20~22 | 19~21 |
| 生产代码缺陷修复 | — | 3 处 | 3 处 |

R2 给**区间而非定值**：`SessionCodec.h` 的依赖闭包会带上 `ActionProtocolCompiler.h` / `GenerationEvent.h` / `BridgeProtocolCodec.h`，按 R2 的闭包口径它们可能一并出榜。**具体降幅须跑完 `tools/architecture_audit.py` 才算数**，此处不预填确定数字。

---

### 阶段 0.5 · Provider 下线（0.5 周）

> **数据迁移门：不设立（P10 裁定，2026-08-07）**
>
> 外部评审第 7 条要求为 Provider 下线补「备份 → dry-run → 影响数量报告 → 回滚脚本 → 观测周期」五步数据迁移门。
> **裁定：不采纳。** 依据：库中存量 Provider 记录**仅 2 条**，判定为无关紧要，
> 不值得为其建立迁移流程 —— 迁移门的成本会超过它保护的资产。
>
> **此裁定的范围严格限于数据库存量数据。** 以下三项**不因此免除**，因为它们与数据量无关：
>
> | 项 | 处置 |
> |---|---|
> | 历史 API 路由 | 需明确返回 **404 / 410 / 兼容跳转** 三者之一，不可留未定义行为 |
> | 配置项与环境变量 | 需明确是否留兼容窗口；若不留，启动时遇旧配置应**显式报错**而非静默忽略 |
> | 指标 / 告警 / Dashboard | 需检查是否仍引用 `nexos` / `openai` 名称，否则删除后看板静默变空 |
>
> **附带好处**：因不做数据变更，**代码回滚即可完全恢复**，无需回滚脚本。
>
> **风险自留声明**：若后续发现存量记录远多于 2 条，本裁定失效，须重新评估。

> **决策（v1.5）**：仅保留 **chayns** 与 **retool**。`openai` / `nexos` 整体删除。
>
> **为什么放在阶段 1 之前**：删除让后续每个阶段的 diff 小一半。先分层再删除，等于为将要删掉的代码做一遍搬迁。

#### 0.5-A 删除 openai —— 独立提交，零风险（解决 P10）

实测状态矩阵：

| 检查项 | 结果 |
|--------|------|
| `src/CMakeLists.txt` 编译登记 | 在编译（第 35 行、第 114 行 include 目录） |
| 工厂注册 | 已注册：`IMPLEMENT_RUNTIME(openai, OpenAiProvider)` |
| `channelManager` 白名单 | **不在**（仅 chaynsapi / nexosapi / retoolapi） |
| `AiApiController` 路由 | **无任何 `/openai/` 路径** |
| `main.cc` 引用 | 无 |

**结论：编译进二进制、注册进工厂，但外部完全无法触达。** 329 行纯死代码。

```
删除  apipoint/openai/OpenAiProvider.cpp   (329 行)
删除  apipoint/openai/OpenAiProvider.h
修改  src/CMakeLists.txt                    第 35 行、第 114 行
```

**门禁**：`grep -ri openai src/` 无输出；全量构建通过；全量用例（**BL-TC** 条）全绿。

#### 0.5-B 清理 nexos 数据 —— 不可回滚，需单独确认

必须**先于**代码删除执行：

```sql
SELECT COUNT(*) FROM accounts WHERE api_name = 'nexosapi';   -- 先确认存量
-- 导出备份后再执行
DELETE FROM accounts WHERE api_name = 'nexosapi';
```

**理由**：`AccountManager::normalizeNexosAccountsInDatabase()`（`accountManager.cpp:622`，由第 593 行在启动流程中调用）
是 nexos 历史账号的用户名 / cookies 规范化迁移逻辑。代码删除后这些记录再无任何路径读写，
将成为永久孤儿数据，并可能在后续 schema 变更时触发约束错误。

> **待确认项**：库中 nexos 账号存量。若为 0，本步骤可整体跳过，风险归零。

#### 0.5-C 删除 nexos 代码 —— 拆两个提交

**提交 A：整体删除（无残留物）**

| 路径 | 行数 |
|------|-----:|
| `apipoint/nexosapi/nexosapi.cpp` + `.h` | 1,334+ |
| `utils/NexosUserAgent.h` | — |
| `utils/NexosRegistrationMailPolicy.h` | 186 |
| `test/test_nexos_user_agent.cpp` | — |
| `test/test_nexos_registration_mail_policy.cpp` | — |

**提交 B：外科手术（9 个文件）**

| 文件 | 具体动作 |
|------|---------|
| `accountManager/accountManager.cpp` | **最重，78 处引用**。删 9 个 Nexos 专属函数：`fetchNexosChatDataByCookie`(224) / `extractNexosCookieHeader`(253) / `decodeNexosSerializedRef`(272,323) / `decodeNexosSerializedInline`(278) / `extractNexosEmailFromChatData`(342) / `normalizeNexosAccountsInDatabase`(622) / `checkNexosToken`(1051) / `updateNexosToken`(1081) / `getNexosToken`(1212)；并将 4 处 `name == chaynsapi \|\| name == nexosapi` 复合条件简化为单值比较（376 / 381 / 405 / 410 行） |
| `controllers/AiApiController.cc` `.h` | 删 `nexosAccountQuota` 方法（270-286）、6 条 `/nexosapi/` 路由注册、`#include <apipoint/nexosapi/nexosapi.h>`、`dynamic_pointer_cast<nexosapi>`、路径前缀分支（38-39） |
| `channelManager/channelManager.cpp` | 白名单去 nexos（第 7 / 28 / 29 / 36 行） |
| `controllers/ChannelController.cc` | 第 13 行白名单 |
| `sessionManager/continuity/OutboundBudget.cpp` | 删 `kFallbackNexos`(11) / `kFallbackMsgNexos`(17) 及两处分支(27/36) |
| `utils/ConfigValidator.cpp` | 删邮件策略校验（第 2 行 include + 167-170 行） |
| `test/stub_account_manager.cpp` | 同步桩函数签名 |
| `test/test_outbound_budget.cpp` | 删 nexos 断言 |
| `src/CMakeLists.txt` | 删第 34 行源文件、第 116 行 include 目录 |

#### 0.5-D 顺手修正误导性命名（解决 P11）

```cpp
ADD_METHOD_TO(AiApiController::chaynsapichat,   "/nexosapi/v1/chat/completions", ...);
ADD_METHOD_TO(AiApiController::chaynsapimodels, "/nexosapi/v1/models", ...);
```

`/nexosapi/*` 复用的其实是 **chayns 的 handler**，靠路径前缀分流 —— 也就是说 `chaynsapichat`
从来就不是 chayns 专属，而是通用 chat handler，名字骗了所有人。

在删除 nexos 路由的**同一个提交**内改名：

| 现名 | 新名 |
|------|------|
| `chaynsapichat` | `chatCompletions` |
| `chaynsapimodels` | `listModels` |

#### 0.5-E 门禁

- `grep -rEi "nexos\|openai" src/` **输出为空**（删除类断言，符合 §8 纪律）
- `src/` 行数由 35,319 降至 ≈31,000（**−12%**）
- `accountManager.cpp` 由 2,600 降至 ≈1,800（**−31%**）
- Provider 白名单由 3 值三元判断降为 2 值
- 阶段 0 安全网四层全绿，chayns 契约回放**逐字节一致**
- 全量构建 + 全量用例（**BL-TC** 条）通过

#### 0.5-F 收益汇总

| 指标 | 下线前 | 下线后 |
|------|-------:|-------:|
| `src/` 总行数 | 35,319 | ≈31,000 |
| Provider 数 | 4 | 2 |
| `accountManager.cpp` | 2,600 | ≈1,800 |
| `session_st` 引用文件数 | 29 | ≈22 |
| 零覆盖高危工作面 | 8,939 行 | ≈6,800 行 |

---

### 阶段 1 · 骨架与地基（1 周）

> **v2.1 状态标注**：本阶段与阶段 3、阶段 5 目前**仅为纲要**（14 / 21 / 8 行），这是**有意暂缓**而非遗漏 ——
> 依 v1.9 决定，阶段边界须待 A 阶段摸底（`RequestAdapters` / `Session` / `accountManager`）与 R2 清理完成后统一重估，
> 现在写细大概率要推翻重写。**细化触发条件**：§0-E-2 的 P0–P3 全部完成且 R2 重跑之后。

- 建立五个 library target 与目录骨架（先空壳，后续逐步搬迁）
- 收敛 include 路径为单一根，批量重写现有 `#include`（脚本可完成大部分）
- 引入 `platform/Result.h`（`std::variant` 实现 + `[[nodiscard]]`）

> 因保持 C++17，**无需任何特性迁移工作**，本阶段维持 1 周。

**门禁**：
- 构建通过
- 阶段 0 安全网全绿
- CI 加入「domain target 不得链接 Drogon」检查

---

### 阶段 2 · 消灭单例（1.5 周，v1.6 重排为 5 批次）

#### 2-0 实测工作面分布

共 **21 个单例类 / 约 180 处调用**，分布极不均匀 —— 前 6 个类占约 60%：

| 单例 | 调用数 | 备注 |
|------|------:|------|
| `AccountManager` | 42 | **11 处在 nexosapi**，阶段 0.5 后降至 **31** |
| `RetoolWorkspaceManager` | 22 | 集中于 `RetoolWorkspaceController` |
| `chatSession` | 13 | 其中 8 处在测试代码 |
| `SessionDbManager` / `ChannelManager` / `ApiManager` | 各 12 | — |
| 其余 15 个类 | 各 ≤9 | 长尾 |

调用点最密集的文件：`AccountController.cc`（26 处）、`RetoolWorkspaceController.cc`（15）、`nexosapi.cpp`（15，阶段 0.5 随文件删除）、`accountManager.cpp`（15）、`main.cc`（14）。

> **不必平均用力**：按类别分批，把风险集中到最后一批。

#### 2-A 批次划分（替代原「一次性迁移顺序」）

| 批次 | 内容 | 工期 | 风险 | 可独立发布 |
|------|------|-----:|------|:---:|
| **2-a** | 删除 `chatSession::instance` 死成员（`Session.h:156` + `Session.cpp:15`） | 10 分钟 | 零 | ✅ |
| **2-b** | C 类无状态单例 → 自由函数 / 普通对象 | 0.3 周 | 低 | ✅ |
| **2-c** | B 类 db 包装类 → 构造注入（7 个 DbManager + ChannelManager） | 0.4 周 | 低 | ✅ |
| **2-d** | 建 `AppContext`，搬迁 `main.cc` 现有初始化顺序 | 0.3 周 | 中 | ✅ |
| **2-e** | A 类 4 个带状态/线程的类，含 `shutdown()` 时序 | 0.5 周 | **高** | ⚠️ |

合计仍为 1.5 周，但**风险全部集中在 2-e**，前四批任一节点都可停下来发布。

#### 2-B A 类内部顺序（强约束）

```
chaynsThreadReaper → ApiManager → chatSession → AccountManager
```

**`AccountManager` 必须排最后**：阶段 0.5 会让 `accountManager.cpp` 瘦身 31%、
并消除 11 处 nexosapi 中的 `AccountManager::getInstance()`。等它先瘦完，注入时要穿的依赖显著减少。

**`chatSession` 排倒数第二**：其 `clearExpiredThread_` 的析构时序是全阶段最高风险点，
需在 A 类前两个类上先验证 `shutdown()` 模式可行。

#### 2-C 每个对象的标准动作

定义 port → infrastructure 实现 → AppContext 注册 → 改完全部调用点 → **删除 `getInstance()`**

A 类额外前置一步：**先补 `shutdown()` 并在 `main.cc` 显式调用，再改所有权**。

#### 2-D 门禁

- ~~`grep -rn "getInstance" src/` 结果为 **0**（删除类断言）~~
  **P10 废止**。两个理由：（1）这把尺子**抓不全** —— `BackgroundTaskQueue::instance()` 用的是
  `instance()` 而非 `getInstance()`，grep 归零它照样是单例；（2）**为指标而指标** ——
  「清零」会诱导为纯工具类和只读配置创建无价值包装器。
- **新门禁（P10，替代「清零」）**：
  1. **业务流程不得经 Service Locator 隐式取依赖** —— `domain` / `application` 层
     出现任何单例访问器（`getInstance` / `instance` / 等价物）即失败；
  2. **有生命周期或持有线程的对象必须由 `AppContext` 显式拥有**（当前 A 类 4 个）；
  3. 无状态纯工具**允许**保留静态自由函数，不计入指标；
  4. 进程级只读配置**允许**受控全局访问，需在 ADR-06 登记豁免清单；
  5. 第三方框架单例（Drogon `app()` 等）**不纳入**本项目指标。
- 领域层单测可在无 DB / 无网络环境下运行
- **A 类新增**：进程收到 SIGTERM 后 5 秒内干净退出，ASan/TSan 无 use-after-free 报告
- 每批次结束全量构建 + 全量用例（**BL-TC** 条）通过

---

### 阶段 3 · Provider 归一（1.5 周）

1. 抽出 `SseFramer` / `RetryPolicy` 并补齐单测
2. 落地 `ProviderBase`
3. 逐个迁移：**retool**（先行验证设计）→ **chayns**（最复杂，含轮询语义与线程回收）

> **v1.5 方案修正**：原计划以 openai / nexos 作为「最简样板」先行验证 ProviderBase，二者已在阶段 0.5 删除，改由 retool 承担验证角色。
>
> 更重要的修正：实测三家 Provider 的**私有方法毫无共性**，抽公共基类只会造出空壳。阶段 3 的目标因此由
> 「归一到公共基类实现」改为 **「归一到瘦接口」** —— 收窄 `APIinterface` 的入参，不再传 `session_st&`，
> 改传 `contracts/GenerationRequest.h`（已存在，189 行，可直接复用）。
>
> 剩下两家差异足够大（chayns 轮询 vs retool 双模式 + workspace），抽出的接口反而更可信 —— 不会为迁就第三家而变形。
3.5 **收敛传输层重试为 `ProviderBase::shouldRetry(const Error&, int attempt)`**（ADR-07 §7.3）

> **定性更正（P7-2 实测）**：`shouldRetry` 在 `src/` 下命中数为 **0** ——
> 它不是既有代码，而是 ADR-07 提出的**待新增虚函数**。
> 此前把它当作「migration-plan 漏写的既有符号」是错的，本条按「新增设计落地」登记。
>
> **候选收敛面（实测分散点）**：
>
> | 位置 | 重试相关行 | 性质 |
> |---|---:|---|
> | `chaynsapi.cpp` | 12 | 轮询语义内自带重试 |
> | `chaynsThreadReaper.cpp` | 4 | 线程回收重试 |
> | `nexosapi.cpp` | 2 | — |
> | `OpenAiProvider.cpp` | 1 | — |
> | `GenerationService.cpp` | 7 | **不是 HTTP 重试**：toolcall 协议校验失败后经 `buildRetryPrompt` 重发（321–337） |
> | `accountManager.cpp/.h` | 7 | `isServerReachable` 的 `maxRetries` 可达性探测 |
>
> ⚠️ **两类「重试」必须分开，不可一并收敛**：
> - **传输层重试**（网络错误 / 429 / 5xx）→ 归 `shouldRetry`，仅在 `sendRequest` 阶段生效（ADR-07 §7.4 硬约束 3）；
> - **语义层重发**（`GenerationService` 的 toolcall 协议纠正、`accountManager` 的可达性探测）→ **不归** `shouldRetry`。
>   它们重发的是**不同的请求内容**，套进传输层重试会改变语义。
>
> 因此真实收敛面仅为 **chayns / nexos / retool 三家 provider 内的传输层重试**；
> `GenerationService` 的 7 处与 `accountManager` 的 7 处**明确排除**。
4. 落地 `ProviderCapabilities`，删除散落的 provider 名称硬判断
5. **收口 P8**：彻底移除 `generate(session_st&)` 的 session 副作用路径

**门禁**：
- 单 Provider < 400 行
- 新增一个 mock provider，验证 200 行内可完成接入

---

### 阶段 4 · 拆解上帝对象（1.5 周）

#### 4-0 （v1.7）实测：这不是未拆分的上帝对象，是**拆了一半且接错线**的对象

**路径纠正**：原写 `sessionManager/generation/GenerationServiceEmitAndToolBridge.cpp` 不存在，
实际为 `sessionManager/core/GenerationServiceEmitAndToolBridge.cpp`（**2,213 行**）。

##### （v1.9）全库 >800 行文件全景：8 个，原方案漏列 2 个

| 文件 | 行数 | 日志活跃度 | RFC 现状 |
|------|---:|---:|------|
| `accountManager.cpp` | **2,600** | 213 条（首） | 已列，零覆盖 |
| `GenerationServiceEmitAndToolBridge.cpp` | 2,213 | 28 条 | 已列 |
| `chaynsapi.cpp` | 1,440 | 活跃 | 已列，零覆盖 |
| `retoolapi.cpp` | 1,352 | 活跃 | 已列，零覆盖 |
| `nexosapi.cpp` | 1,334 | 活跃 | 阶段 0.5 删除 |
| `XmlTagToolCallCodec.cpp` | 1,314 | 2 条 | v1.7 新增 |
| **`RequestAdapters.cpp`** | **1,255** | **36 条（第 4）** | ❌ **v1.9 前未列** |
| **`Session.cpp`** | **1,221** | **17 条** | ❌ **v1.9 前未列** |

**两处前提纠正**：

1. RFC 自 v1.0 起将 god 文件称为「最大单点」，**不成立** —— `accountManager.cpp` 大 387 行，且同样零覆盖、日志量居首。
2. `RequestAdapters.cpp` + `Session.cpp` 合计 **2,476 行活跃主路径代码**，v1.9 前方案未提及。

##### （v1.9）拆分必须区分为两个动作

god 文件 9 个函数中，4 个（503+554+258+234 = **1,549 行，占文件 70%**）均超 R3 阀值。

| 动作 | 含义 | 依赖关系 |
|---|------|------|
| **A：函数内拆分** | 将 >200 行函数拆为职责单一的小函数 | **必须先做** |
| **B：文件拆分** | 将函数按职责分配到新文件 | 依赖 A 完成 |

> 先做 B 会得到「一个文件里装一个 554 行函数」—— 文件行数达标但可读性未改善，属于**假性完成**。
> 原方案未区分这两个动作，工期估算因此偏乐观。

##### 体量基线（原方案缺此数）

| 文件 | 行数 | 是否超 800 行目标 |
|------|-----:|:---:|
| `core/GenerationServiceEmitAndToolBridge.cpp` | 2,213 | ❌ |
| `tooling/XmlTagToolCallCodec.cpp` | 1,314 | ❌ **原方案漏列** |
| `tooling/ToolCallValidator.cpp` | 687 | ✅ |
| `core/GenerationService.cpp` | 531 | ✅ |
| `tooling/BridgeProtocolCodec.cpp` | 513 | ✅ |

##### 关键发现：组件已抽出，但生产从未切过去

`tooling/` 下已有 10 个专职组件、`actionProtocol/` 2 个。但其中两个是**残废实现**：

| | 生产实际执行 | 测试实际执行 |
|---|---|---|
| 实现位置 | `GenerationService::` 静态成员 | `toolcall::` 自由函数 |
| `normalizeToolCallArguments` | ~258 行：union type 处理、`toolsRaw` 优先、schema 查找、别名、默认值 | **41 行**：仅 JSON 解析 + 非对象包装 |
| `generateForcedToolCall` | ~234 行：解析 `tool_choice` JSON、`makeBridgeToolName` 定向 | **51 行**：**完全忽略 `tool_choice`**，无脑取第一个工具 |

**名字查找证据链**：调用点（717 / 798 / 808 行）位于 `GenerationService::emitResultEvents` 函数体内、
**无 `toolcall::` 限定**、文件内**无 `using namespace toolcall`** → C++ 名字查找先命中类作用域静态成员，
永远不会走到命名空间版。对照组：1648 行的 `toolcall::applyStrictClientRules` **带限定**，但它位于成员函数内部而非调用点（见下方 v1.8 校正）
。

> `ToolCallNormalizer.h` 的注释描述的是 god 版的行为，而它自己的 `.cpp` 并未实现 —— **头文件在说谎**。

##### （v1.8 校正）`applyStrictClientRules` 不是「切换成功」，而是「转发壳」

v1.7 将其描述为「唯一切换成功的案例」，**此判断错误**。实际链路：

```
emitResultEvents:908  →  applyStrictClientRules(...)          ← 非限定，命中成员版
                            ↓
GenerationService::applyStrictClientRules  (1643–1649，共 6 行)
                            ↓
                        toolcall::applyStrictClientRules   ← 真正的 200 行实现
```

生产确实执行到了组件，但不是靠「改调用点」，而是靠「成员函数内部转发」。
这反而提供了一个比 v1.7 原方案更低风险的搬迁路径（见下）。

##### （v1.8）待查清单已结案

**`ToolDefinitionEncoder`（29 行）—— 确认为空壳，且是四个里最危险的一个**

组件版仅：编码工具定义 → 生成触发标记 → 清空 `tools`。成员版（1660–2213，**554 行**）额外承担：

- `capabilitiesForClient` 能力 IR 推导 + `isStrictToolClient` 判定
- `resolveBridgeWireFormat` / `resolveBridgeFormatFallback` 读配置定协议格式
- **Codex 专属：清空上游 `systemPrompt`**（源码注释明确：保留会诱发拒绝或纯文本输出）
- `definition_mode` full/compact 开关、描述截断（默认 160 字符）

调用点 `GenerationService.cpp:203`，位于 `executeGuardedWithSession`（142 行起）—— 同样是类作用域内非限定调用。

**`ActionProtocolAdapter`（75 行）—— 健康，非空壳**

`adaptForCapabilities` 被 `BridgeProtocolCodec.cpp:179` 真实调用；无同名成员竞争；
逻辑有实质内容（截断 → 补收尾工具 → 清文本，注释声明顺序不可调）；
`test_action_protocol.cpp` 走 `adaptForClient`，与生产的 `adaptForCapabilities` 是同一实现的两个入口 —— **有效覆盖**。

**仍未定论**：`XmlTagToolCallCodec.cpp`（1,314 行）处于 ToolBridge 主路径（被 `ToolDefinitionEncoder` 直接调用），
但日志中 `XML` 仅 2 次 —— 无法区分「不打日志」与「解析分支未触发」，待阶段 0 补埋点后重测。

##### 方向修正：搬迁方向与直觉相反

权威实现是 **god 文件里那份**（功能完整、生产在跑）。

**v1.8 采用转发壳模式（照搬 `applyStrictClientRules` 的现成写法），取代 v1.7 的四步方案**：

| 步 | 动作 |
|---|------|
| 1 | 把成员版函数体**整体搬进组件 `.cpp`**，覆盖残废实现 |
| 2 | 成员版**缩成 6 行转发壳** |
| 3 | **调用点一行不改** |
| 4 | （可选、放最后）统一删转发壳并给调用点加 `toolcall::` 限定 |

相比 v1.7 方案的优势：

- 调用点零改动 → 名字查找行为不变 → **无隐蔽语义漂移**
- 测试 include 不变，但所测实现变成真货 → **测试失败即真实缺陷暴露**（预期信号，非回退）
- 每个函数可**独立提交、独立回滚**

反向做法（把调用点直接切到现有 `tooling/` 残废版）**会造成功能回退** —— `tool_choice` 定向调用将全部失效。

##### （v1.8）日志实证：安全网该往哪儿投

数据源：`build/logs/aiapi.log`，827 行，2026-08-07 08:49–15:38（约 7 小时真实运行）。

| 观察 | 数据 | 含义 |
|------|------|------|
| `通道 chaynsapi supportsToolCalls: 0` | 反复出现 | **chayns 恒定走 ToolBridge 文本桥**，非边缘路径 |
| `accountManager.cpp` | 213 条，居首 | 与「零覆盖 + 最大改动面」重合，风险坐实 |
| god 文件 | 28 条 | 主路径确凿在跑 |
| `RooCode` / `Kilo` / `ActionProtocol` | **各 0 条** | 严格客户端路径本窗口未触发，覆盖需靠构造用例而非录制 |
| `namespaceToolBridgeEnabled=0` | 启动日志 | 命名空间桥接关闭，`makeBridgeToolName` 分支不活跃，**可降优先级** |
| `nexosapi` | 仍初始化 / 队列 2 账号 / 正在校验 | **阶段 0.5 删除前必须先清账号数据**（印证 v1.5 不可回滚风险项） |

> **结论**：`transformRequestForToolBridge` 是每请求必经的 550 行零覆盖代码，
> 优先级**应高于** `emitResultEvents`。原方案未提及此函数。

##### 对工期的影响

匿名命名空间的 20 个辅助函数（约 460 行，49-513 行）无外部调用者，**搬走零风险**，建议排最前；
但 `appendRetoolChannelSpecialRules` 在 448 行前向声明、492 行才定义，说明内部已有调用顺序纠缠，需整堆搬迁。
`emitResultEvents`（约 500 行）是响应输出主路径，**必须在 chayns 契约回放逐字节通过后才能动**。

---

**4a. `GenerationServiceEmitAndToolBridge.cpp` (2213 行) → 流水线**
逐 stage 抽出，每抽一个跑一次阶段 0 安全网。

**4b. `accountManager.cpp` (2600 行) → 五个组件**

| 新组件 | 职责 | 所属层 |
|--------|------|--------|
| `AccountRepository` | 纯持久化，委托 dbManager | infrastructure |
| `AccountRotator` | 轮转与选取策略（纯逻辑，可测） | domain |
| `TokenRefresher` | 刷新与过期判定 | infrastructure |
| `AccountRegistrar` | 自动注册流程（重 IO） | infrastructure |
| `AccountHealthTracker` | 可用性 / 封禁状态 | domain |

**门禁**：最大源文件 < 800 行。

---

### 阶段 5 · 收口（0.5 周）

- 错误模型四套合一（`ProviderResult` / `Errors` / `ErrorEvent` / `ErrorStats`）
- 前向声明 + Pimpl 降低头文件耦合
- 引入 ccache；评估 unity build
- 更新 `README.md` 架构章节；本 RFC 状态改为「已实施 (Accepted)」

---


---

## 0-F. 依赖环实测与阶段 0.7「解环」（v2.3 新增）

### 先纠正一次我自己的错误测量

v2.2 之前用「include 路径前缀」判定模块归属，测出 3 条边、**0 个环**，据此认为 ADR-02 可直接落地。
**该判据本身有缺陷** —— 项目里绝大多数 include 写的是 basename（这正是 ADR-03 要治的病），前缀匹配自然什么都查不到。

改用 **basename → 模块的全量映射**重测（同名头文件歧义数 = **0**，映射唯一，结论可靠）：
**13 条模块间依赖边，3 个双向环。**

### 为什么这件事必须排在阶段 1 之前

ADR-02 靠 `target_link_libraries` 在编译期强制分层，而 **CMake 不允许 static library 之间循环依赖**。
环不解开，阶段 1 的五个 target 拆不出来 —— 这是**硬约束，不是风格偏好**。

### 三个环逐条定性（已定位到文件:行号）

| 环 | 具体 include 点 | 性质 | 处置 |
|---|---|---|---|
| `apiManager` ↔ `apipoint` | `ApiManager.h:2` → `apipoint/APIinterface.h`；`chaynsapi.h:5` → `apiManager/ApiFactory.h` | **弱环** | **前向声明即可断**，见下 |
| `dbManager` ↔ `metrics` | `ErrorStatsDbManager.h:11` → `metrics/ErrorEvent.h`；`ErrorStatsService.h:6` → `dbManager/metrics/ErrorStatsDbManager.h` | **弱环** | **移动一个文件即可断** |
| `apipoint` ↔ `sessionManager` | `APIinterface.h:5` / `chaynsapi.h:4` → `Session.h`；**`Session.cpp:6` → `chaynsapi.h`** | **一半合法 + 一条真违规** | 拆两半处理 |

#### 环 1 · `apiManager ↔ apipoint`：弱环，0.5 天

`ApiManager.h` 只以 `std::shared_ptr<APIinterface>` 形式持有（成员、构造参数、返回值），**从不解引用**。
`shared_ptr` 的声明与拷贝**不要求完整类型**，因此把 `#include` 降级为前向声明、include 下沉到 `.cpp` 即可。

> **本仓已有先例**：`Apicomn.h:9` 就写着 `class APIinterface;` —— 这个做法在项目里已经用过，不是新引入的技巧。

另一方向（`chaynsapi.h` → `ApiFactory.h`）是 **Provider 自注册**模式，`nexosapi` / `retoolapi` / `openai` 四个实现都这么写。
注意：**阶段 0.5 会删掉 openai 与 nexos**，届时该方向只剩 2 处，成本进一步下降。

#### 环 2 · `dbManager ↔ metrics`：弱环，0.5 天

唯一成因是 **`ErrorEvent.h` 放错了地方**。它是一个纯数据模型，却住在 `metrics/`（服务层）里，
导致 `dbManager` 为了用这个数据结构反向依赖了 `metrics`。

**处置**：把 `ErrorEvent.h` 移到 `domain/model/`（按 §3 目标结构），两侧同时向下依赖它，环自动消失。
**这不需要任何接口倒置，只是把文件放回它该在的层。**

#### 环 3 · `apipoint ↔ sessionManager`：其中三条边里，只有一条是真违规

这是我复审中最值得记下的一点 —— **不能整个环一起判死刑**：

| 边 | 方向 | 按 ADR-01 是否合法 |
|---|---|---|
| `APIinterface.h:5` → `Session.h` | Layer 1 → Layer 2 | ✅ **合法**。infrastructure 依赖 domain 正是依赖倒置期望的方向 |
| `chaynsapi.cpp` → `HistoryReplayBudget.h` / `OutboundBudget.h` | Layer 1 → Layer 2 | ✅ **合法**，同上 |
| `GenerationService.cpp:16` / `EmitAndToolBridge.cpp:18` → `apipoint/ProviderResult.h` | Layer 2/3 → Layer 1 | ⚠️ **形式违规，但成因是文件放错层** |
| **`Session.cpp:6` → `chaynsapi.h`** | **Layer 2 → Layer 1 具体实现** | ❌ **真违规**。domain 直接依赖一个具体 Provider |

**拆解结论**：

- `ProviderResult.h` 与 `ErrorEvent.h` 同病 —— 它是**纯聚合体**（v2.2 已用 `static_assert` 实证），除 jsoncpp 外零依赖，
  本就属于 `domain/model/`。**移动文件即可消除这两条边**，同时 §0-E-3 的 H2 测试用例位置也随之确定。
- 唯独 `Session.cpp:6 → chaynsapi.h` 是**真正的 DIP 违规**：`chatSession` 单例直接 new 了一个具体 Provider。
  这条边**必须靠 `IChatProvider` port + 组合根注入才能断**，与**阶段 2「消灭单例」是同一件事**，
  在阶段 0.7 强行处理会做两遍 —— **本版明确不排期，转阶段 2 批次**。

### 阶段 0.7 · 解环（新增，排在阶段 1 之前）

| 任务 | 估时 | 阻断的环 |
|---|---:|---|
| C1 `ApiManager.h` include 降级为前向声明 | 0.5 天 | 环 1 |
| C2 `ErrorEvent.h` 移入 `domain/model/` | 0.5 天 | 环 2 |
| C3 `ProviderResult.h` 移入 `domain/model/` | 0.5 天 | 环 3 的两条形式违规边 |
| C4 环检测脚本化，纳入 R 系列规则（**R4**）+ CI 门禁 | 0.5 天 | 防回归 |
| **小计** | **2 天 ≈ 0.4 周** | 3 环中的 2 个完全解开，第 3 个降为单边 |

> **C4 是这四项里最重要的一项。** 前三项是一次性清理，若无门禁，环会以同样的方式长回来 ——
> 这正是 ADR-02「文档约束必然腐化，构建约束不会」那句话所指的情形。

**残留**：`Session.cpp → chaynsapi.h` 单向边保留至阶段 2。也就是说 **阶段 1 的 target 拆分需容忍 `domain → infrastructure` 一条临时边**，
建议用 CMake 注释显式标注为「阶段 2 前的已知例外」，而不是悄悄放行。

### 工期影响

| | 周 |
|---|---:|
| v2.2 口径（含 A 类 + H 类） | 9.1 |
| **阶段 0.7 解环** | **+0.4** |
| **合计（v2.3 阶段性口径，已被附录 A 取代）** | ≈ 9.5 周 |

> ⚠️ **这不是当前总工期**。本表止于 v2.3（解环），不含 N1–N9。
> **全项目总工期唯一权威值见本文附录 A 末尾的工期表**（当前 ≈ 10.3 周）。

> 不含：P4、`BackgroundTaskQueue`、B 类 14 条、以及 ADR-05 补充中新拆出的 `Result` 铺开工作量。

---


---

## 附录 A · 并发与网络模型实测（原 RFC-001 §0-G，v2.4）

> 决策部分已抽出为 [`decisions/ADR-08-concurrency-and-shutdown.md`](./decisions/ADR-08-concurrency-and-shutdown.md)。
> 本附录保留**实测证据链与处置项**，因其本质是迁移任务的输入，而非决策本身。


RFC-001 v2.3 之前**完全没有涉及并发模型** —— ADR-01～07 无一条提到线程。本节补上，并记录一个**优先级高于全部重构工作的高危缺陷**。

### 先纠正我自己的两处错误

1. 上一轮我用 `--include=*.cpp` 扫 `src/controllers/`，而 controller 全部是 **`.cc`** 后缀 —— 探测结果全空，我据此说「controller 未直接调用 provider」。**判据无效，结论作废**。
2. 我说 `BackgroundTaskQueue::shutdown()` 无人调用。**这条我说错了** —— `main.cc:382` 在 `app().run()` 返回后确实调用了。真正的问题在时序，不在缺失（见下）。

### 实测线程清单：四类，共 18 条常驻线程

| 类别 | 数量 | 来源 |
|---|---:|---|
| Drogon/Trantor 事件循环 | **4** | `config.json: number_of_threads` |
| BackgroundTaskQueue 工作线程 | **8** | `kDefaultWorkerThreads`；`main.cc:216` 显式 `start()` |
| 裸 `detach()` 常驻线程 | **5** | `accountManager.cpp` 4 个 + `Session.cpp:739` 1 个 |
| `chaynsThreadReaper` worker | **1** | `std::thread worker_` |

### 网络请求：29 处 `sendRequest`，**全部同步阻塞，异步回调 0 处**

`HttpClient::newHttpClient` 25 处，**无一传入 loop 参数** —— 每次请求新建 client，无连接复用。

---

### ⚠️ 高危：非流式路径在事件循环线程上同步阻塞

**这是本次复审发现的最严重问题，严重度高于此前所有条目。**

完整证据链：

| 环节 | 证据 |
|---|---|
| handler 运行在 IO 事件循环线程 | `AiApiController.h` 用 `ADD_METHOD_TO` 注册，Drogon 默认在 loop 线程回调 |
| 非流式分支**同步**执行生成 | `AiApiController.cc:180` / `:337` 就地构造 `GenerationService`，直接 `runGuarded(...)` 并等返回值 |
| 生成链路含阻塞 IO | `runGuarded` → `provider->generate` → `chaynsapi::postChatMessage`（`:485/:611/:787/:801/:908` 五处同步 `sendRequest`） |
| 还含显式睡眠 | `chaynsapi.cpp:956` 轮询退避 `sleep_for(pollingDelayForElapsed(...))` |

**后果**：一个非流式请求会独占一条事件循环线程**整个上游耗时**（LLM 场景为秒级至分钟级）。
事件循环只有 **4** 条 —— **4 个并发非流式请求即可让整个服务失去响应**，包括 `/health` 与 `/metrics`（健康检查会误判为宕机）。

### 而流式路径是**对的**

`AiApiController.cc:216-221` 的做法完全正确：

1. `newAsyncStreamResponse` 回调内先 `IoLoopResponseStream::create(...)` 把流绑定到当前 IO loop
2. 再 `BackgroundTaskQueue::instance().enqueue("chat_stream_generation", ...)` 把生成推到后台
3. 事件循环立即 `callback(resp)` 返回

> **同一个 controller 里，流式做对了、非流式做错了。**
> 这说明它不是设计判断失误，而是**流式改造时漏改了非流式分支** —— 属于遗漏，修复成本很低。

---

### 停机时序：队列关了，但线程没关全，且顺序是错的

`main.cc` 在 `app().run()` 返回后：AccountManager（注释自承「无独立后台线程停机接口，由进程退出统一回收」）→ `BackgroundTaskQueue::shutdown()`。

| 缺口 | 实测 |
|---|---|
| `chaynsThreadReaper::stop()` **写了但没接线** | 实现完整（`stopRequested_` + `wakeCv_.notify_all()` + `join()`），全项目**调用点 0 处** |
| `Session.cpp:739` 清理线程 | `detach()` + `while(true)`，无停止路径 |
| `accountManager` 4 个 detach 线程 | 同上，含 `sleep_for(hours(5))`、`sleep_for(hours(3))` |
| **顺序错误** | Reaper 与清理线程仍在运行时，`BackgroundTaskQueue` 已 `shutdown()` |

最后一条会引发一个**具体的、可推演的缺陷**：`enqueue()` 内含「若未启动则自动启动」分支，
而 `shutdown()` 会把 `started_` 复位为 `false`。于是**停机后**任何仍存活的线程调用 `enqueue`，
都会**重新拉起 8 条工作线程** —— 停机流程执行完，线程数反而回升。

这是「懒启动」与「shutdown 复位状态」两个各自合理的设计**组合**出来的缺陷。

---


### 处置项（按性质归类，不改重构阶段划分）

> **状态列口径（P10 新增）**：✅ = 已提交且全量 **BL-TC** 用例通过；🟡 = 部分落地；⬜ = 未开始。
> 本表是**任务台账**，状态随代码变动；ADR 不承载此进度（见 RFC 顶部权威分工）。

| 编号 | 任务 | 估时 | 状态 | 归属 |
|---|---|---:|:--:|---|
| **N1** | **非流式路径改为 enqueue 到后台队列** | 0.5 天 | ✅ `1630e08` | 已作为热修先行落地 |
| N2 | `enqueue` 在 shutdown 后 fail-fast | 0.5 天 | ✅ `1630e08` | 含用例 `#174 BackgroundTaskQueue_ShutdownIsIrreversible` |
| N3 | `Reaper::stop()` 接线到停机路径 | 0.5 天 | ✅ `1630e08` | `main.cc:389/400/410` |
| N4 | 5 个 detach 线程 → `runEvery` / 可停 worker | — | 🟡 | `Session.cpp:723` 已改可停成员线程；**其余 4 处未复核** |
| N5 | `HttpClient` 复用 + loop 绑定 | — | ⬜ | 并入阶段 3（`ProviderBase` 落地） |
| **N6** | **`SessionExecutionGate::cancelAll()`**（停机 S0.5 广播取消） | 0.5 天 | ⬜ | **依赖 N8 先出结论**（见 F4 撤回） |
| **N7** | **8 处裸 `sleep_for` → `backgroundSleep`**（ADR-08 决策 3 收紧） | 0.5 天 | ⬜ | 热修可行；含 3 个例外 |
| **N8** | **取消检查点普查**（P10 新增，只出报告不改代码） | 0.5 天 | ⬜ | **N6 的前置**；见下节撤回说明 |
| **N9** | **`BackgroundTaskQueue` 四态状态机 + `EnqueueResult`**（ADR-08 决策 4 收紧） | 1 天 | ⬜ | **改公开接口，非热修**；随阶段 1；12 个调用点适配 |

> **N1/N2/N3 的提交粒度问题**：三项打包在同一个 commit `1630e08`，不满足「每项单独通过全量测试」。
> 已落地，不追溯重切；**后续 N6/N7/N8 一项一提交**。

> **N1 是本文档中唯一建议插队到重构之前的项**。理由：它与架构重构无关（改 20 行以内），
> 但当前状态下 4 个并发非流式请求即可打死服务 —— 不应该等 9 周重构完才修。
> N3 更是**只需接一行调用**，实现早已写好。

### N6 补充：`cancelAll()` 的可行性已实测（P7-4）

**定性更正**：`cancelAll` 同样在 `src/` 下命中 **0** —— 是 ADR-08 §8.5 提出的待新增接口，非既有符号。

**挂载点真实存在**：`src/sessionManager/core/SessionExecutionGate.h:98`，单例，持有
`slots_`（`mapMutex_` 保护）与每槽 `SessionSlot{ mutex, currentToken, executing }`。

**可复用的现成件**：`CancellationToken`（同文件 :30）已具备 `cancel()` / `isCancelled()`，
`std::atomic<bool>` + acquire/release，语义完备；`tryAcquire` 的 `CancelPrevious` 分支（:133-135）
**已经在用 `slot->currentToken->cancel()`** —— `cancelAll` 只是把这一行从「单槽」扩到「全槽遍历」。

**实现成本**：约 10 行，无新抽象、无新依赖。

> ⚠️ **P10 撤回**：下文原称 N6「实现成本最低、收益明确」。**实现成本的判断仍成立，收益判断已撤回** ——
> 见本节末「P10 撤回与 N8 前置」。

```cpp
// 建议实现：持 mapMutex_ 遍历，只碰 token，不碰 slot->mutex（避免与执行中的槽位死锁）
void cancelAll() {
    std::lock_guard<std::mutex> mapLock(mapMutex_);
    for (auto& kv : slots_) {
        if (kv.second->currentToken) kv.second->currentToken->cancel();
    }
}
```

**三条实现约束**：

1. **只取 `mapMutex_`，绝不去锁 `slot->mutex`** —— 正在执行的槽位本就持有它，
   停机路径若去争这把锁会直接把 S0.5 卡死，而 S0.5 的意义恰恰是「立即、不阻塞」。
2. **`cancelAll()` 后不得清空 `slots_`** —— 在跑的 Pipeline 仍持有 token 的 `shared_ptr`，
   且 S2/S3 的 join 尚未完成；清 map 会让 `release()` 找不到槽位。清理留给进程退出。
3. **幂等**：`CancellationToken::cancel()` 是 `store(true)`，重复调用无害，
   故 `cancelAll()` 可被 `AppContext::shutdown()` 与析构兜底路径重复调用。

> ~~**与 ADR-08 §8.4 时序表的一致性**：S0.5 标注「立即 / 无超时」，上述实现满足该约束 ——~~
> ~~唯一阻塞点是 `mapMutex_`，而持有它的路径（`release` / `isExecuting` / `cleanup`）均为 O(1) 或短临界区。~~

### P10 撤回与 N8 前置

上面这句**是错的**，两处都错：

1. **`cleanup()` 不是 O(1)**。`SessionExecutionGate.h:183-197` 在持有 `mapMutex_` 时遍历全部 `slots_`
   并构造 `toRemove` 向量，是 **O(n)**，n 上限由 `maxIdleSlots = 1000` 决定。
   故 `cancelAll()` 可能等一次最长 1000 元素的遍历，**「立即 / 无阻塞」的表述作废**，
   改为：**「有界等待」——最坏 = 一次 1000 元素遍历 + 一次 map 遍历，量级微秒，可接受但必须如实标注**。
2. **更严重：设标志 ≠ 取消成功**。全库取消检查点实测**仅 2 处**，且都在同一文件：
   `GenerationService.cpp:221` 与 `:347`。上游 HTTP 请求、chayns 轮询等待、重试退避、
   SSE 输出、DB 操作**均无检查点**。这意味着卡在 30 秒 HTTP 超时里的 Session，
   `cancelAll()` 置了标志也得等它自己回来 —— **N6 对停机时间的实际改善接近于零**。

**因此新增 N8（0.5 天，只出报告）**：普查五类阻塞边界（上游 HTTP / 轮询 / 退避 / SSE / DB）
各自是否有取消检查点、能否插入、插入后语义如何。**N8 出结论前 N6 不实施** ——
否则只是加了一个看起来在做事、实际不缩短停机时间的调用。

**锁顺序约束（补记）**：`cancelAll` 只取 `mapMutex_`；已知持 `mapMutex_` 的路径为
`release` / `isExecuting` / `cleanup`，均**不再取 `slot->mutex`**，故不存在与执行中槽位的锁序反转。
`tryAcquire`（:124）对 `slot->mutex` 用的是 `try_to_lock`，不参与阻塞式锁序。

### 工期

| | 周 |
|---|---:|
| v2.3 口径 | 9.5 |
| N1+N2+N3（1.5 天） | +0.3 |
| N6（0.5 天，P7-6 新增；**待 N8 结论后重估**） | +0.1 |
| N7（0.5 天，P8-5 新增） | +0.1 |
| **N8 取消检查点普查（0.5 天，P10 新增）** | **+0.1** |
| **N9 队列状态机（1 天，P10 新增）** | **+0.2** |
| **合计** | **≈ 10.3 周** |

> **本表是全项目工期的唯一权威来源**（RFC 顶部 F1 口径）。RFC §6 / §9 只是分解视图。

---



---

## N8 结论回写（2026-08-07）

N8 普查已完成，报告见 `doc/adr/reports/N8-cancellation-audit.md`。要点：

1. **N6 不实施（证伪，非推迟）**。Provider 接口 `generate(session_st&)` 不携带取消令牌，
   取消状态在物理上到不了阻塞点；停机时 cancel 全部 token 只产生日志。
   降级为阶段 3 子项，待接口带 token 后重评。
2. **N10 新增 · HTTP 超时统一（0.5 天）**。29 处 `sendRequest` 有 21 处无显式超时，
   已有 8 处取值 30.0 / 300.0 混用无常量。这是当前架构下唯一能生效的止损手段，
   优先级高于 N6。随阶段 3 做（届时调用点已收敛）。
3. **N11 新增 · SSE 断连回传取消（0.5 天）**。`IoLoopResponseStream.h:127` 的 `send()`
   返回值已能表达客户端断连，但未回传给 Gate。五个边界中唯一不需改接口即可接线的。
4. **DB 边界明确排除**，不留待办：单条 SQL 毫秒级，非停机瓶颈，改异步波及全部 DbManager。
5. **N7 实施模板已确定**：以 `accountManager.cpp:448` 的 `backgroundSleep()`
   （`wait_for` + 谓词，停机可唤醒）为准，不另起炉灶。同文件另有 8 处裸 `sleep_for` 待替换。

**工期**：N8 实耗 0.1 天，N6 −0.5，N10 +0.5，N11 +0.5，净 +0.5 天。
**总工期 10.3 → 10.4 周。**
