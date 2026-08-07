# RFC-001 aiapi 架构重构方案

> **数字真值源**：本文中所有架构审计数字（R1/R2/R3 命中数、测试用例与断言数、测试链接覆盖数）**不再内联硬编码**，改为引用 `doc/adr/architecture-baseline.md` 的基线编号 `BL-*`。
> 基线由 `tools/architecture_audit.py` 生成，`--selftest` 通过方为有效。若本文数字与基线冲突，**以基线为准**。
>
> **工期真值源**（P10 新增）：所有阶段工期、增量与总工期**以 `migration-plan.md` 为准**。
> 本文 §6 / §9 两张表是**分解视图**，说明工期如何构成；与 migration-plan 冲突时以后者为准。
>
> **文档权威分工**（P10 冻结）：
> `architecture-baseline.md` = 审计数字唯一真值源 ｜ `migration-plan.md` = 执行计划 / 工期 / 验收口径唯一真值源
> 本文 = 目标、范围、总体方案与 ADR 索引 ｜ `decisions/ADR-*.md` = 已接受的架构决策，**不记录滚动任务进度**
> `CHANGELOG.md` = 版本演进与纠错留痕，**不作为当前事实来源**。

| 项目 | 内容 |
|------|------|
| 编号 | RFC-001 |
| 状态 | 草案 (Draft) |
| 版本 | v2.6 |
| 日期 | 2026-08-07 |
| 范围 | `src/` 全量约 39,000 行 C++（阶段 0.5 下线后减少约 4,300 行） |
| Provider 范围 | **仅保留 chayns + retool**；nexos / openai 下线（v1.5 决策） |
| 语言标准 | **C++17**（与现状一致，见 ADR-04） |

---


> **v2.6（P4 文档拆分）**：本文已由 1794 行收敛为**决策文档**，不再承载执行细节。
> 所有被移出的内容**一字未改**地保留在派生文档中，见下表。

## 文档族导航

| 文档 | 承载内容 | 何时读 |
|---|---|---|
| **本文** RFC-001 | 背景、目标、ADR 索引、目录结构、风险、纪律、工期 | 想知道**为什么**这么改 |
| [`decisions/`](./decisions/) | ADR-01~08，每条独立成文 | 想知道**某条决策**的完整理由 |
| [`migration-plan.md`](./migration-plan.md) | 阶段 0~5 全部任务、门禁、并发实测与处置项 | 想知道**这周做什么** |
| [`interface-drafts.md`](./interface-drafts.md) | 关键接口草案（C++17） | 想知道**代码长什么样** |
| [`architecture-baseline.md`](./architecture-baseline.md) | `BL-*` 审计数字唯一真值源 + R2 分类法口径 | 想引用**任何数字** |
| [`CHANGELOG.md`](./CHANGELOG.md) | v1.0~v2.6 演进与自我纠错留痕 | 想知道**某个结论何时被推翻** |

> **硬性纪律**：本文及任何派生文档**不得内联硬编码审计数字**，只能引用 `BL-*` 编号。
> 冲突时以 `architecture-baseline.md` 为准。

## 0. 背景与问题陈述

项目由增量迭代而成，当前 14 个模块 / 约 39K 行 / 20+ 单例。

> **v1.5 决策**：Provider 收敛为 **chayns + retool** 两家，`nexos` 与 `openai` 在**阶段 0.5** 整体下线。
> 删除前置于所有结构性改造 —— 这不是插队，而是让后续每个阶段的改动面缩小 30~50%。

### 0.1 现状代码量分布

| 模块 | 文件数 | 行数 |
|------|-------:|-----:|
| sessionManager | 49 | 13,530 |
| apipoint | 17 | 5,857 |
| dbManager | 19 | 4,716 |
| controllers | 25 | 4,172 |
| accountManager | 2 | 2,829 |
| utils | 8 | 1,223 |
| metrics | 5 | 913 |
| retoolWorkspace | 5 | 457 |
| managedAccount | 8 | 354 |
| apiManager | 5 | 246 |
| channelManager | 2 | 225 |

### 0.2 问题清单

| 编号 | 问题 | 客观证据 |
|------|------|----------|
| P1 | 依赖关系不可见 | CMake 将全部 14 个模块目录加入 include path，源码中 `#include "Session.h"` 不带路径 |
| P2 | 全局可变状态泛滥 | **实测 21 个单例类 / 约 180 处 `getInstance()` 调用**，分布见 §6 阶段 2；单例按状态性质分三类，处理方式不同 |
| ~~P2b~~ | ~~`chatSession` 存在两份实例~~ | **v1.6 撤销：经核实为误判**。`Session.h:156` 的 `static chatSession *instance`（`Session.cpp:15` 定义为 `nullptr`）**从无任何读写**，被 `getInstance()` 内的局部 static（`Session.h:174`）同名遮蔽。不是双实例，是一个死成员变量制造的阅读歧义。风险等级由「并发灾难」降为「顺手清理」 |
| P3 | 依赖方向倒挂 | `APIinterface::generate(session_st&)` 使基础设施层直接修改领域对象 |
| P4 | 接口职责混杂 | `APIinterface` 同时承担生成 / 生命周期 / 上游会话状态三类职责 |
| P5 | 上帝对象 | `accountManager.cpp` 2600 行、`GenerationServiceEmitAndToolBridge.cpp` 2213 行 |
| P6 | Provider 代码分散 | 下线后为 chayns 1440 + retool 1352 = 2792 行。**实测三家私有方法并不同构**（chayns 轮询式 / retool workflow+agent 双模式 / nexos 账号选择重载），原「同构约 60%」的估计不成立 —— 详见 §6 阶段 3 的方案修正 |
| P7 | 构建耦合 | 单一 `add_executable`，无模块边界；`Session.h` 被 12 处包含，改动触发大范围重编 |
| P8 | 上轮重构未收口 | `ProviderResult` 已定义，但调用链仍走 session 副作用；`plans/aiapi-refactor-design.md` 已不存在 |
| **P9** | **语言标准由探测决定，存在漂移风险** | 见 §0.4，主程序与测试目标可能编译在不同标准下 |
| **P10** | **存在已编译但不可触达的死代码** | `OpenAiProvider` 已 `IMPLEMENT_RUNTIME(openai, ...)` 注册进工厂、且在 `CMakeLists.txt` 第 35 行参与编译，但 `channelManager` 白名单不含 openai、`AiApiController` 无任何 `/openai/` 路由 —— 329 行代码外部无法触达 |
| **P11** | **路由 handler 命名误导** | `/nexosapi/v1/chat/completions` 复用的是 `AiApiController::chaynsapichat`，靠路径前缀（`AiApiController.cc:38`）分流；该函数实为通用 chat handler，名字使其看似 chayns 专属 |
| **P12** | **测试与生产路径脱钩（v1.7 提出，v1.8 校正）** | 全库 47 个 `tooling/`+`actionProtocol/` 导出自由函数中，**4 个**与 `GenerationService::` 成员同名。其中 `generateForcedToolCall`（成员 234 行 vs 组件 51 行）、`normalizeToolCallArguments`（258 vs 41）**测试空转**；`transformRequestForToolBridge`（成员 554 行 vs 组件 29 行）**无任何测试且为每请求必经路径**；`applyStrictClientRules` 成员版仅 6 行转发壳，**健康** |
| **P13** | **`ApiManager` 查询接口带写副作用且存在 UB（v2.0 新增）** | `getApiInfoByModelName` 对空 `priority_queue` 调 `top()`（`operator[]` 先建空队列）→ **未定义行为**；全部查询与开关接口用 `operator[]`，查不到会静默插入 nullptr 条目 → map 无限膨胀；`updateApiInfo` 为**空函数体**，调用方以为更新成功；`flushModelnameApiQueueMap` pop 后 push 同一元素、计数未变 → 疑似空操作 |

### 0.3 上一轮重构为何没有完成

`ProviderResult.h` 的注释引用了 `plans/aiapi-refactor-design.md`，该文件当前已不在仓库中，
而 `APIinterface::generate` 仍然接收 `session_st&`。

推断：新旧两条路径并存（结构化返回值 vs. session 副作用），但缺少「删除旧路径」的强制关卡，
导致新结构只是**叠加**而非**替换**，复杂度不降反升。

> **本方案的核心纪律**：每个阶段的完成标志是「旧代码已删除」，而非「新代码已可用」。

### 0.4 语言标准现状核查（P9）

实测结论：**项目当前实际编译在 C++17**。

| 检查项 | 实测结果 |
|--------|----------|
| 实际编译参数 | `-std=c++17` |
| `HAS_ANY` / `HAS_STRING_VIEW` | 均为 1 |
| `HAS_COROUTINE` | **空**（g++ 12 需 `-fcoroutines` 或 `-std=c++20` 才能找到 `<coroutine>`） |
| 本机编译器 | g++ 12.2.0 (Debian) |
| Docker 基础镜像 | ubuntu:22.04（默认 g++ 11） |
| Drogon 依赖要求 | `cxx_std_17`（FindFilesystem.cmake） |
| C++17 特性使用量 | `std::optional` 61 处、`if constexpr` 28、结构化绑定 28、`string_view` 27、`variant` 2；共约 146 处 / 50 文件 |

**发现的两处隐患**：

1. **标准由探测结果决定**：`src/CMakeLists.txt` 依据 `<coroutine>` 是否存在在 20/17/14 之间自动降级。
   当前恰好落在 17；若换用支持 `<coroutine>` 的编译器（如 clang + libc++，或 g++ 配合不同默认标准），
   同一份代码会被编译成 C++20，**产生跨机器语义差异**。
2. **主程序与测试目标标准不一致**：`src/test/CMakeLists.txt` 中 `if (NOT CMAKE_CXX_STANDARD) set(... 20)`。
   独立配置测试目标时会取 **C++20**，而主程序是 17。这意味着测试与产物可能不在同一标准下编译。

> 因此 ADR-04 的价值不在于「改标准」，而在于**消除探测、把标准钉死在 17 并统一到测试目标**。


### 0.4b P9 修复结果（已完成，commit `efb4003`）

| 验证项 | 修复前 | 修复后 |
|--------|--------|--------|
| 主程序编译单元 `-std=` | c++17（探测得出，不稳定） | **c++17 x 120，硬编码** |
| 测试目标独立配置 `-std=` | **c++20 x 57** | **c++17 x 57** |
| 探测逻辑残留 | `CheckIncludeFileCXX` + 三级降级 | 已全部移除 |
| `target_compile_features` | 无 | `cxx_std_17`（随 target 传播） |
| 测试结果 | — | **BL-TC** 用例 / **BL-TA** 断言全绿（见 `architecture-baseline.md` §1） |
| 主程序构建 | — | 通过 |

> P9 已闭环，阶段 0 中该项可直接勾除。

### 0.5 测试覆盖基线（实测，2026-08-07）

现有 **BL-TF** 个测试文件 / **BL-TC** 用例 / **BL-TA** 断言，采用 Drogon 内置测试框架（数字真值见 `architecture-baseline.md` §1）。
> **v2.5 状态**：用例数与断言数已重测并**迁出本文** —— 见 `architecture-baseline.md` §1 的 `BL-TF` / `BL-TC` / `BL-TA`。本文不再保留会漂移的副本。
以「主程序源文件是否被测试目标链接」为口径统计（粗粒度，非行覆盖率）：

| 模块 | 已覆盖文件 | 未覆盖文件 | 已覆盖行 | 未覆盖行 | 覆盖率 |
|------|-----------:|-----------:|---------:|---------:|-------:|
| sessionManager | 18 | 4 | 7,272 | 2,859 | 71.8% |
| apipoint | 2 | 5 | 493 | 4,639 | **9.6%** |
| dbManager | 2 | 7 | 1,011 | 2,790 | 26.6% |
| controllers | 4 | 7 | 1,115 | 2,134 | 34.3% |
| accountManager | 0 | 1 | 0 | 2,600 | **0.0%** |
| metrics | 2 | 0 | 460 | 0 | 100% |
| retoolWorkspace | 0 | 2 | 0 | 252 | 0.0% |
| managedAccount | 0 | 3 | 0 | 222 | 0.0% |
| tools / utils / apiManager | 4 | 0 | 529 | 0 | 100% |
| channelManager | 0 | 1 | 0 | 174 | 0.0% |
| **合计** | **32** | **30** | **10,880** | **15,670** | **41.0%** |

#### 关键结论：已有的安全网质量不错，但恰好没盖住要动刀的地方

现有测试集中在 sessionManager 的**纯逻辑子模块**（continuity / tooling / actionProtocol）
与 controllers/sinks，这些正是本次重构中要迁往 `domain/` 的部分——说明它们已具备可测性，
迁移风险低。

而重构改动最大的五个文件，**全部零覆盖**：

| 文件 | 行数 | 覆盖 | 对应阶段 |
|------|-----:|:----:|----------|
| `accountManager/accountManager.cpp` | 2,600 | 无 | 阶段 4b |
| `sessionManager/core/GenerationServiceEmitAndToolBridge.cpp` | 2,213 | 无 | 阶段 4a |
| `apipoint/chaynsapi/chaynsapi.cpp` | 1,440 | 无 | 阶段 3 |
| `apipoint/retoolapi/retoolapi.cpp` | 1,352 | 无 | 阶段 3 |
| `apipoint/nexosapi/nexosapi.cpp` | 1,334 | 无 | **阶段 0.5 删除**，不再补测 |

合计 8,939 行、占全库约 23%。其中 `nexosapi.cpp`（1,334 行）在阶段 0.5 直接删除，
加上 `accountManager.cpp` 中约 800 行 nexos 专属逻辑一并移除，**实际需要补测的零覆盖工作面降至约 6,800 行**。

> 这是把删除前置的最大理由：**删掉的代码不需要写测试**。

> **这是当前最大的单点风险**：改动量最集中的代码，恰好完全没有回归保护。
> 阶段 0 的核心任务因此明确为——**为这五个文件建立可断言的行为基线**。

---

## 1. 目标与非目标

### 1.1 目标

- **G1** 依赖关系显式化，且由构建系统强制
- **G2** 领域逻辑可在无数据库、无网络条件下单元测试
- **G3** 新增一个 Provider 的成本从约 1300 行降到 200 行以内
- **G4** 单个源文件不超过 800 行
- **G5** 全程可发布，任意时点主干可上线
- **G6** 语言标准确定、可复现，主程序与测试一致

### 1.2 非目标（本次明确不做）

- 不更换 Web 框架（继续 Drogon）
- 不更换数据库
- 不新增业务功能
- 不追求 100% 测试覆盖率，只保证关键路径
- **不变更语言标准**（保持 C++17，既不升 20 也不降 14）

### 1.3 量化验收指标

| 指标 | 现状 | 目标 |
|------|------|------|
| `getInstance()` 调用点（业务代码，实测） | **约 180 处 / 21 个类** | 0 |
| 其中 nexos 贡献（阶段 0.5 消化） | 11 处 | — |
| 最大源文件行数 | 2600 | < 800 |
| 单 Provider 行数 | ~1370 | < 400 |
| 领域层单测是否依赖 IO | 是 | 否 |
| 主程序 / 测试标准是否一致 | 否（17 vs 20） | 是（均 17） |
| 全量构建时间 | 阶段 0 测定 | 下降 30% |
| 增量构建（修改 `Session.h`） | 阶段 0 测定 | 下降 60% |

---


## 2. 架构决策记录（ADR）索引

每条 ADR 已独立成文，可单独修订、单独作废，不再随本文一起漂移。
完整理由与补充条款见 [`decisions/`](./decisions/)。

| 编号 | 标题 | 状态 | 解决的问题 |
|---|---|---|---|
| [ADR-01](./decisions/ADR-01-layered-architecture.md) | 四层架构 + 横切 `platform` + 依赖倒置 | 已接受 | P1 / P7 |
| [ADR-02](./decisions/ADR-02-cmake-enforced-layering.md) | 用 CMake target 强制分层 | 已接受 | P1 |
| [ADR-03](./decisions/ADR-03-single-include-root.md) | include 路径收敛为单一根 | 已接受 | P7 |
| [ADR-04](./decisions/ADR-04-cxx17-fixed.md) | 固定 C++17，移除标准探测 | **已落地** `efb4003` | P9 |
| [ADR-05](./decisions/ADR-05-result-type.md) | `Result<T, Error>` 统一，跨层禁止抛异常 | 已接受 | P8 |
| [ADR-06](./decisions/ADR-06-composition-root.md) | 单例改组合根注入，不做兼容层 | 已接受 | P2 |
| [ADR-07](./decisions/ADR-07-provider-template-method.md) | Provider 模板方法 + 可组合管线 | 已接受 | P6 |
| [ADR-08](./decisions/ADR-08-concurrency-and-shutdown.md) | 并发模型与停机时序 | 已接受 | P12 / P14 |

> **术语统一口径**：架构分层 = **4**；CMake target = **5**（4 层 + `platform`）。
> `platform/` 是横切设施，**不是第五层** —— 四层都可依赖它，它不依赖任何层。

## 3. 目标目录结构

```
src/
├── platform/                  # 跨层通用设施
│   ├── Result.h               Result<T, Error>（基于 std::variant）
│   ├── Logging.h
│   └── Config.h
│
├── domain/                    # Layer 2 — 零外部依赖，可独立编译测试
│   ├── session/               Session, SessionKey, Turn, Message
│   ├── tooling/               ToolCall, Validator, Normalizer,
│   │                          XmlCodec, DefinitionEncoder, ForcedCall
│   ├── continuity/            ContinuityResolver, HistoryReplayBudget,
│   │                          OutboundBudget, TextExtractor
│   ├── policy/                StrictClientRules, ActionProtocol
│   ├── model/                 GenerationRequest, GenerationEvent, Usage
│   └── ports/                 ★ 接口定义（依赖倒置核心）
│       ├── IChatProvider.h
│       ├── IProviderLifecycle.h
│       ├── IUpstreamThreadStore.h
│       ├── IAccountRepository.h
│       ├── IChannelRegistry.h
│       ├── ISessionStore.h
│       ├── IMetricsSink.h
│       └── IClock.h
│
├── application/               # Layer 3 — 编排，无 IO
│   ├── ChatCompletionUseCase
│   ├── ResponsesUseCase
│   ├── AccountUseCase
│   ├── ChannelUseCase
│   └── pipeline/              ★ 生成流水线各 stage
│
├── infrastructure/            # Layer 1 — 实现 ports
│   ├── provider/
│   │   ├── ProviderBase.*     共性：重试/SSE/超时/错误映射
│   │   ├── SseFramer.*        独立可测
│   │   ├── RetryPolicy.*      独立可测
│   │   ├── chayns/  retool/          # v1.5：nexos / openai 已下线
│   ├── persistence/           原 dbManager/*，实现 I*Repository
│   ├── account/               Repository / Rotator / Refresher / Registrar / HealthTracker
│   ├── metrics/
│   └── http/                  HttpClient 封装、UA、浏览器指纹
│
├── transport/                 # Layer 4 — Drogon
│   ├── controllers/
│   ├── filters/               AdminAuthFilter, RateLimitFilter
│   └── sinks/                 Chat/Responses × Json/Sse
│
├── AppContext.h / AppContext.cpp   ★ 组合根
└── main.cc                    仅：读配置 → 构建 AppContext → 启动
```

---


## 4. 关键接口草案

已外移至 [`interface-drafts.md`](./interface-drafts.md)（238 行，含 `APIinterface` 拆解、`ProviderBase`
模板方法、生成流水线、组合根、`Result<T, Error>` 的 C++17 实现）。

**草案不是契约**：签名可在不违反 ADR 的前提下调整；若调整会违反某条 ADR，先改 ADR。

## 5. CMake 分层强制（解决 P1 / P7 / P9）

```cmake
cmake_minimum_required(VERSION 3.5)
project(aiapi CXX)

# ADR-04：钉死标准，移除 check_include_file_cxx 探测与三级降级
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(aiapi_platform       STATIC ${PLATFORM_SRC})
add_library(aiapi_domain         STATIC ${DOMAIN_SRC})
add_library(aiapi_application    STATIC ${APPLICATION_SRC})
add_library(aiapi_infrastructure STATIC ${INFRASTRUCTURE_SRC})
add_library(aiapi_transport      STATIC ${TRANSPORT_SRC})

# ADR-03：唯一 include 根
foreach(t platform domain application infrastructure transport)
  target_include_directories(aiapi_${t} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_features(aiapi_${t} PUBLIC cxx_std_17)
endforeach()

# ADR-02：依赖方向由链接关系强制表达
target_link_libraries(aiapi_domain         PUBLIC  aiapi_platform)
#  ↑ 关键：domain 不链接 Drogon / PostgreSQL / OpenSSL
#         任何人在领域层 #include <drogon/...> 会立即链接失败

target_link_libraries(aiapi_application    PUBLIC  aiapi_domain)

target_link_libraries(aiapi_infrastructure PUBLIC  aiapi_domain
                                           PRIVATE Drogon::Drogon
                                                   PostgreSQL::PostgreSQL
                                                   OpenSSL::SSL OpenSSL::Crypto)

target_link_libraries(aiapi_transport      PUBLIC  aiapi_application
                                           PRIVATE Drogon::Drogon)

add_executable(aiapi main.cc)
target_link_libraries(aiapi PRIVATE aiapi_transport aiapi_infrastructure)

# P9：测试目标必须复用同一标准，禁止再出现 set(CMAKE_CXX_STANDARD 20) 兜底
add_subdirectory(test)
```

> **这一步是整个方案的地基**。domain 不链接 Drogon，违规依赖在编译期即被拦截，
> 无需人工 Code Review 把关。

---


## 6. 分阶段执行计划

已外移至 [`migration-plan.md`](./migration-plan.md)。本文仅保留阶段清单与工期，任务级细节以该文为准。

| 阶段 | 名称 | 工期 | 关键门禁 |
|---|---|---:|---|
| 0 | 安全网 | 1.5 周 | 三层安全网建立；R2 A 类补断言 |
| 0.5 | Provider 下线 | 0.5 周 | openai / nexos 代码与数据清理 |
| 0.7 | 解环 | 0.4 周 | 三个依赖环拆除，**必须早于阶段 1**（P10：由 0.3 校正为 migration-plan 的 0.4） |
| 1 | 骨架与地基 | 1.0 周 | 5 个 CMake target 建立，分层由链接强制 |
| 2 | 消灭单例 | 1.5 周 | 5 批次；A 类**先补 `shutdown()` 再改所有权** |
| 3 | Provider 归一 | 1.0 周 | **接口收窄**（ADR-07 收窄版模板方法），仅需协调 2 家实现 |
| 4 | 拆解上帝对象 | 1.5 周 | `APIinterface` 按 ports 切分 |
| 5 | 收口 | 0.5 周 | 全量门禁通过 |

> **表内合计 7.9 周（阶段基线，含 0.7）**。P10 修正：本表此前逐行相加为 8.3 周，与 §9 的 7.5 周不一致
> —— 原因是阶段 3 两表各写 1.5 / 1.0，且 §9 整行缺 0.7 解环。现两表已对齐，工期总量仍以 migration-plan 为准。

> **热修插队**：`migration-plan.md` 附录 A 的 **N1 + N2 + N3**（共 1.5 天）与架构改动无关，
> 建议在阶段 0 之前独立落地 —— 其中 N1（非流式改 enqueue）修复的是**全文档最高危项**。

## 7. 风险与对策

| 风险 | 等级 | 对策 |
|------|:----:|------|
| 重构期间行为漂移 | 高 | 阶段 0 四层安全网（chayns 真实回放 + 轮询时序 + 特性化 + 冒烟）；重构 commit 禁止夹带功能变更 |
| 又一次「只加不删」半途而废 | 高 | 每阶段门禁均为**删除类断言**（grep 计数、行数上限） |
| chayns 上游线程语义复杂，迁移易错 | 高 | 放在 Provider 迁移最后；先用前三个验证 ProviderBase 设计 |
| 标准漂移（换编译器后变 C++20 / 测试用 20） | 中 | ADR-04 钉死 17 + `target_compile_features` + CI 校验 `-std=` |
| **`chatSession` 注入化改变线程析构时序，退出时 use-after-free** | **高** | 批次 2-e 单列；先补 `shutdown()` 再改所有权；ASan/TSan 门禁；在 A 类前两个类上先验证模式 |
| **`accountManager.cpp` 2,600 行 / 零覆盖 / 日志量居首，比 god 文件更大** | **高** | 阶段划分需重评；不得再以「god 文件是最大单点」作为优先级依据 |
| `chaynsapi.cpp` 内存在 **865 行单函数**（全库最大）且零覆盖 | **高** | 先函数内拆分再谈其他；纳入 R3 基线监控 |
| `BridgeHelpers` 188 行、扇入 5（最高）、测试 0 | 中 | R2 新发现；`generateRandomTriggerSignal()` 在其中，阶段 0 补测试 |
| **`transformRequestForToolBridge` 554 行每请求必经且零测试** | **高** | 阶段 0 新增 1.5 天特性化；完成前禁止触碰该函数 |
| 以「.cpp 行数小」误判组件为空壳 | 中 | `ActionProtocolAdapter` 75 行但健康；审计一律以调用图 + 同名竞争定义为据 |
| **现有用例空转，安全网给出虚假信心** | **高** | 阶段 0 新增 0-E 测试有效性审计；阶段验收添加「空转用例数 = 0」硬条件 |
| 搬迁方向选错导致功能回退（切到残废版） | 高 | 4-0 明确权威实现为 god 文件版；搬迁后测试失败视为预期信号而非回退 |
| include 批量重写引入编译错误 | 中 | 脚本改写 + 全量构建验证，单独一个 commit 便于回滚 |
| 长命分支导致合并地狱 | 中 | Strangler Fig，每日合主干，单阶段不超过 1.5 周 |
| 提交历史不可追溯 | 中 | 立即改用规范 commit message（现状多为 `08070939` 类时间戳） |
| **P13：`ApiManager::getApiInfoByModelName` 对空队列调 `top()` 属未定义行为** | **高** | **当前唯一已定位的生产期 UB**，非重构引入。按 §0-E-2 的 P2 处置：先写替身测试记录现状，再修。修复前不得扩大 `ApiManager` 调用面 |
| **R2 中 B 类须先改 CMake，链接闭包不可预测** | **高→中** | 仍是最大工期不确定源，但 v2.2 已由 18 条收窄至 **14 条**（剥离 3 条纯头 + 排除 1 条误报），并按 impl 行数分 B1/B2/B3 三层（见 §0-E-3）。B3 三条（2601/1441/532 行）仍**禁止打包报工期** |
| **`Result<T>` 从 0 处用法铺开是大规模签名变更** | **高** | 实测 `Result<` 现有用法为 **0**、`catch` 达 **100**。ADR-05 的 v2.3 补充已定转换边界（infra 出口），但铺开工作量被压在阶段 1 的 1.0 周内不现实，须单独拆分立项 |
| **模块间存在 3 个双向依赖环，阻断 ADR-02 的 target 拆分** | **高** | CMake 不允许 static library 循环依赖。v2.3 已实测定位到文件:行号，新增**阶段 0.7 解环**（2 天）；其中 2 个为弱环可立即断开，`Session.cpp → chaynsapi.h` 属真 DIP 违规，转阶段 2 |
| `GenerationServiceEmitAndToolBridge.cpp` 分析篇幅与风险倒挂 | 中 | 2213 行、含 SSE 时序 + XML 增量解析 + 工具桥接状态机，是全项目语义最复杂处，文档仅提及 8 次；而 2600 行的 `accountManager`（CRUD + 轮换，路径清晰）提及 30 次。§4.3 Pipeline 拆解前须补专项分析 |
| **非流式请求在事件循环线程上同步阻塞** | **极高** | `AiApiController.cc:180/:337` 在 loop 线程同步 `runGuarded`，链路含 5 处同步 `sendRequest` + `sleep_for` 轮询。事件循环仅 **4** 条，4 个并发非流式请求即可使服务（含 `/health`）失去响应。同一 controller 的流式分支（`:216-221`）已正确 enqueue 到后台 —— 属流式改造时漏改非流式，见 N1，**建议热修插队** |
| **停机后 `enqueue` 会重新拉起线程池** | **高** | `shutdown()` 将 `started_` 复位，而 `enqueue()` 含「未启动则自动启动」分支；停机时 Reaper 与清理线程仍在运行且会 enqueue → 停机流程结束后线程数回升。见 ADR-08 决策 5（N2） |
| 5 个 detach 常驻线程 + `Reaper::stop()` 未接线 | 中 | `Reaper::stop()` 实现完整但**全项目 0 处调用**；`Session.cpp:739` 与 `accountManager` 4 处为 `detach + while(true) + 长 sleep`（含 `hours(5)`/`hours(3)`），无停止路径，进程退出时 DB 写可能丢。正确先例已存在：`ErrorStatsService.cpp:40` 用 `runEvery`。见 N3/N4 |
| ~~R2 中 4 条 `impl=0` 纯头组件可能是规则误报~~ **（v2.2 结案，见 §0-E-3）** | 中→低 | 逐条实证：**仅 `Apicomn.h` 1 条为真误报**（13 行，2 个前向声明 + 单值 enum，无可断言行为）；另 3 条是**真实缺口**，其中 2 条零链接成本 |

---

## 8. 工程纪律（重构期强制）

1. **重构 commit 只含结构变更**，行为变更单独提交
2. **Commit message 规范**：`refactor(provider): 抽取 SSE 分帧器`
3. **CI 硬门禁**：
   - 单文件行数 <= 800
   - `getInstance(` 新增数 = 0
   - domain target 依赖白名单校验
   - 编译参数必须为 `-std=c++17`，且不得出现 C++20 特性（禁用清单见 ADR-04）
   - 主程序与测试目标标准一致
   - 阶段 0 安全网必须通过
4. **每阶段结束打 tag**，便于回滚与对比基线指标

---

## 9. 时间线汇总

| 阶段 | 周期 | 累计 | 核心交付 |
|------|-----:|-----:|----------|
| 0 安全网 | **1.5 周** | 1.5 | chayns 契约回放（真实 har）+ 特性化 + 基线指标 + 覆盖率 ≥60% |
| **0.5 Provider 下线** | **0.5 周** | **2.0** | **删除 openai + nexos，−4,300 行** |
| **0.7 解环**（P10 补入） | **0.4 周** | **2.4** | 三个依赖环拆除，必须早于阶段 1 |
| 1 骨架地基 | 1.0 周 | 3.4 | 五层 target + include 收敛 + Result |
| 2 消灭单例 | 1.5 周 | 4.9 | AppContext；**依赖不再经 Service Locator 获取**（口径见 migration-plan 2-D） |
| 3 Provider 归一 | **1.0 周** | 5.9 | 接口收窄，仅需协调 2 家实现（原 1.5 周 / 4 家） |
| 4 拆上帝对象 | 1.5 周 | 7.4 | 生成流水线 + 账号组件（accountManager 已瘦身 31%） |
| 5 收口 | 0.5 周 | 7.9 | 错误模型统一、构建优化 |

**表内合计 7.9 周** —— 这是**阶段基线**，不含 v1.7 之后新增的专项工作与附录 A 热修项。
P10 修正：补入 0.7 行并与 §6 对齐；旧值 7.5 周因缺该行而偏低。

##### v2.1 工期对账（此前 §9 与 §10 互相矛盾，本节为唯一口径）

| 来源 | 增量 | 说明 |
|---|---:|---|
| §9 阶段基线（v2.1 口径，缺 0.7 行） | 7.5 周 | 历史值，保留备查 |
| §9 阶段基线（**P10 口径**，含 0.7） | **7.9 周** | 上表八个阶段之和 |
| v1.7 新增 0-E 测试有效性审计 | **+1 天** | 已发生 |
| v1.8 净增（`transformRequestForToolBridge` 特性化 +1.5 天，审计 −0.5 天，阶段 4 转发壳 −0.5 天） | **+0.5 天** | 已发生 |
| 对账后总工期（v2.1 口径） | ≈ 7.9 周 | 历史值。⚠️ 与上一行 P10 阶段基线**数值巧合但构成不同**，勿混用 |
| **当前总工期** | **≈ 10.0 → 10.1 周** | **以 `migration-plan.md` 附录 A 工期表为准**（F1 权威口径） |
| §0-E-2 A 类（**BL-R2-A** 条，P0–P4） | **未计入** | 见下表 |
| R2 B 类 18 条 | **不可估算** | 见 §7 风险表 |

> **v2.1 纠正**：§9 这张表自 v1.6 起未随变更记录更新，长期显示 7.5 周，而 §10 的 v1.8 条目写的是 7.9 周，
> 二者矛盾持续了三个版本。**以 7.9 周为准**；上表保留 7.5 是因为它确实是「阶段基线」，删掉会丢失分解结构。

> **P10 再纠正**：v2.1 只对齐了 §9 与 §10，**没发现 §6 与 §9 也不一致**（阶段 3 = 1.5 vs 1.0，§9 缺 0.7 行）。
> 本轮已对齐两表。同时确立：**RFC 不再是工期的权威来源**，任何总工期以 migration-plan 为准 ——
> 根因不是数字漂移，而是「工期」既不是 `BL-*` 审计数字、又未被任何文档声明归属，处于三不管地带。

##### §0-E-2 的工期归属（v2.1 明确）

| 项 | 归属 | 估时 | 依据 |
|---|---|---:|---|
| P0 `SessionCodec` | 阶段 0 内 | **2 天** | 10 用例 + 3 处生产改动 + 4 条变异验真 |
| P1 `TextExtractor` | 阶段 0 内 | **0.5 天** | 22 行 impl、零依赖 |
| P2 `ApiManager` + 修 P13 | 阶段 0 内 | **1.5 天**（v2.2 已解除条件） | 8 个纯虚函数替身 + 修 4 项缺陷 |
| P3 `ApiFactory` | 阶段 0 内 | **0.5 天** | 复用 P2 替身 |
| P4 `SessionDbManager` | **暂不排期** | — | 涉真实 DB 与异步队列，非纯函数，需先定替身策略 |
| **A 类小计（P0–P3）** | | **4.5 天 ≈ 0.9 周** | |

> ~~**P2 的 1.5 天带前置条件**：须先探明 `provider::ProviderResult` 能否默认构造。~~
>
> **v2.2 结案 —— 条件已解除，P2 的 1.5 天转为无条件估时。**
>
> 判定方式不是读代码推测，而是**编译期实证**：四条 `static_assert` 分别检查
> `is_default_constructible` / `is_copy_constructible` / `is_move_constructible` / `is_copy_assignable`，
> 经 `g++ -std=c++17 -fsyntax-only` **全部通过（rc=0）**。
>
> 原因：`ProviderResult`（`src/apipoint/ProviderResult.h:100`）是纯聚合体 —— 7 个数据成员全部带默认初值
> （`statusCode = 200`、`meta{Json::objectValue}` 等），无用户声明构造函数、无 `const`/引用成员、无 `= delete`。
> 因此 `ApiManager` 替身可零成本返回 `ProviderResult{}`。
>
> **这是全文估时中唯一的带条件项，现已归零 —— §9 估时表目前不含任何待定前置条件。**

**含 §0-E-2 A 类（P0–P3）后：≈ 8.8 周**（7.9 + 0.9）。

**再含 §0-E-3 的 H 类 2 条后：≈ 9.1 周**（8.8 + 0.3）。仍不含 P4、`BackgroundTaskQueue`、以及 B 类 14 条。

> 阶段 0.5 新增的 0.5 周由阶段 3 的缩短（1.5 → 1.0 周）完全抵消，**总工期不变**。
> 换言之，Provider 下线是**自付费**的 —— 它省下的工作量不少于它本身的成本。

> 阶段 0 由 1.0 周上调至 1.5 周（原计划未含 har 脱敏工具链与特性化测试工作量），后续阶段整体顺延 0.5 周。

> **最小可行子集**：若资源受限，阶段 0 + 0.5 + 1 + 2（4.5 周）即可获得约 80% 收益
> —— 依赖可见、领域可测、单例清零。阶段 3~5 可按需延后。

---


## 10. 变更记录

已外移至 [`CHANGELOG.md`](./CHANGELOG.md)，含 v1.0~v2.5 全部演进与自我纠错留痕。

