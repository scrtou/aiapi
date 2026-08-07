# RFC-001 aiapi 架构重构方案

| 项目 | 内容 |
|------|------|
| 编号 | RFC-001 |
| 状态 | 草案 (Draft) |
| 版本 | v2.2 |
| 日期 | 2026-08-07 |
| 范围 | `src/` 全量约 39,000 行 C++（阶段 0.5 下线后减少约 4,300 行） |
| Provider 范围 | **仅保留 chayns + retool**；nexos / openai 下线（v1.5 决策） |
| 语言标准 | **C++17**（与现状一致，见 ADR-04） |

---

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
| 测试结果 | — | 159 用例 / 657 断言全绿 |
| 主程序构建 | — | 通过 |

> P9 已闭环，阶段 0 中该项可直接勾除。

### 0.5 测试覆盖基线（实测，2026-08-07）

现有 24 个测试文件 / 3,820 行 / 159 用例 / 657 断言，采用 Drogon 内置测试框架。
> **v2.0 状态**：`test_bridge_helpers.cpp` 已加入 `TEST_SOURCES`（第 25 个文件），用例数与断言数**待重测后回填** —— 本表数字仍为 v1.9 基线，不做估算。
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

## 2. 架构决策记录 (ADR)

### ADR-01 采用四层架构 + 依赖倒置

```
Layer 4  transport      Drogon Controllers / Filters / Sinks
Layer 3  application    UseCase 编排，无 IO
Layer 2  domain         纯逻辑 + 接口定义（零外部依赖）
Layer 1  infrastructure Provider / DbManager / HttpClient / Clock
```

依赖方向严格自上而下；Layer 1 实现 Layer 2 定义的接口（ports）。

**理由**：领域层无外部依赖是可测试性的前提，其余目标均依赖此条。

---

### ADR-02 用 CMake target 强制分层，而非依靠约定

拆分为独立 library target，通过 `target_link_libraries` 的可见性（PUBLIC / PRIVATE）
在**编译期**阻断非法依赖。

**理由**：P1 的根因是「约定没有强制力」。文档约束必然腐化，构建约束不会。

---

### ADR-03 include 路径收敛为单一根

`target_include_directories` 只保留 `src/`，所有 include 写全相对路径：

```cpp
#include "domain/session/Session.h"   // 而非 #include "Session.h"
```

**理由**：让依赖在源码中肉眼可见，Code Review 可直接发现跨层引用。

---

### ADR-04 固定 C++17，移除标准探测

**决策**：明确采用 C++17，删除 `check_include_file_cxx` 探测与 20/17/14 三级降级逻辑。

```cmake
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

同时**必须**修正 `src/test/CMakeLists.txt` 中的 `set(CMAKE_CXX_STANDARD 20)` 兜底，
改为与主程序一致的 17，或直接删除该兜底、由顶层统一提供。

**理由**：
1. 项目实测已在 C++17（约 146 处 C++17 特性），与现状一致，**零迁移成本**。
2. Drogon 依赖本身要求 `cxx_std_17`。
3. 探测式降级会导致不同机器编译出语义不同的产物（见 §0.4），必须消除。
4. 主程序 17 / 测试 20 的不一致会让单测无法真实反映产物行为，必须统一。

#### 允许使用的特性（明确白名单）

本方案的设计直接建立在 C++17 之上，可放心使用：

| 特性 | 在本方案中的用途 |
|------|------------------|
| `std::optional` | 可空返回值；`ProviderResult::usage` 保持现状 |
| `std::variant` | `Result<T, Error>` 的判别式实现（见 §4.5） |
| `std::string_view` | Provider 分片解析零拷贝（`parseChunk`） |
| `if constexpr` | Pipeline / 编解码的编译期分支 |
| 结构化绑定 | 遍历 map、多返回值解包 |
| 内联变量、折叠表达式、`std::string_view` 字面量 | 通用 |
| `[[nodiscard]]` | **强制**用于 `Result<T>`，防止错误被静默忽略 |

#### 明确禁止（C++20 及以上）

`concepts`、`ranges`、`std::expected`、`std::span`、`coroutine`、designated initializers、
`std::format`、三路比较运算符。

> CI 门禁：编译参数必须包含 `-std=c++17`，且不得出现 `-std=c++20` / `gnu++`。

---

### ADR-05 Result 类型统一，跨层禁止抛异常

统一为 `Result<T, Error>`，基于 `std::variant` 实现（C++17 原生可用，见 §4.5）。
现有 `ProviderResult` 收敛为 `Result<Generation>` 的特化使用。

**理由**：当前存在 `ProviderResult` / `Errors` / `ErrorEvent` / `ErrorStats` 四套并行的错误表达。

---

### ADR-06 单例改为组合根注入，一次性替换、不做兼容层

每个模块迁移时直接改完所有调用点，**不保留** `getInstance()` 兼容壳。

**理由**：保留兼容壳正是上一轮重构半途而废的机制（见 §0.3）。

#### v1.6 补充一：组合根无需从零设计，`main.cc` 已是雏形

实测 `main.cc:235-241` 的初始化序列**已经是显式、集中、有序**的：

```cpp
ChannelManager::getInstance().init();
AccountManager::getInstance().init();
RetoolWorkspaceManager::getInstance().init();
ApiManager::getInstance().init();
metrics::ErrorStatsService::getInstance().init(statsConfig);
```

依赖顺序现成、无循环、无隐藏初始化。`AppContext` 的构建**本质上是把这段已有顺序换一种写法**：

```cpp
// 之前
AccountManager::getInstance().init();
// 之后
ctx.accountManager = std::make_shared<AccountManager>(ctx.accountDb, ctx.channelMgr);
```

> **结论修正**：阶段 2 的难点**不在组合根设计**（原方案高估），而在 180 处调用点的机械替换 —— 是体力活，不是设计活。

#### v1.6 补充二：单例分三类，处理方式必须区分

原方案默认「全部注入」。实测 21 个类性质差异极大：

| 类别 | 特征 | 代表 | 处理方式 |
|------|------|------|----------|
| **A 类** | 持有可变状态，**部分带后台线程** | `chatSession`（`session_map` / `context_map` / `mutex_` / `clearExpiredThread_`）、`ApiManager`（优先队列 + 映射表）、`chaynsThreadReaper`、`AccountManager` | 真注入，**必须配套显式 `shutdown()`** |
| **B 类** | 仅持有一个 db 指针 | `ChannelManager`（唯一成员 `shared_ptr<ChannelDbManager>`） | 退化为普通对象，构造成本近零 |
| **C 类** | 实质无状态，方法直通 db | `ManagedAccountService`（private 段几乎为空） | 改自由函数，单例身份纯属惯性 |

**A 类的关键风险**：`chatSession` 持有 `std::thread clearExpiredThread_` 成员。
注入化会改变析构时序 —— 进程退出时若线程晚于 `session_map` 销毁，将访问已析构对象。
因此 A 类迁移**必须先补 `shutdown()` 显式停线程，再改所有权**，顺序不可颠倒。

---

### ADR-07 Provider 采用模板方法 + 可组合管线

共性（重试、SSE 分帧、超时、错误映射、账号轮转、指标上报）下沉到基类；
差异（构造请求、鉴权、解析分片、错误映射）留纯虚函数。

---

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

## 4. 关键接口草案（C++17）

### 4.1 拆解 APIinterface（解决 P3 / P4）

现状：单一接口 7 个方法，混合三类职责。拆为三个：

```cpp
// domain/ports/IChatProvider.h —— 只管生成，输入不可变
struct ProviderCapabilities {
    bool nativeToolCalls   = false;
    bool streaming         = true;
    bool serverSideHistory = false;   // chayns / retool 有上游线程
    int  maxContextTokens  = 0;
};

class IChatProvider {
public:
    virtual ~IChatProvider() = default;
    [[nodiscard]] virtual Result<Generation> generate(const ProviderRequest& req) = 0;
    [[nodiscard]] virtual ProviderCapabilities capabilities() const = 0;
};
```

```cpp
// domain/ports/IProviderLifecycle.h —— 生命周期与健康检查
class IProviderLifecycle {
public:
    virtual ~IProviderLifecycle() = default;
    virtual void initialize() = 0;
    [[nodiscard]] virtual Result<void> refreshTokens() = 0;
    [[nodiscard]] virtual Result<ModelCatalog> listModels() = 0;
};
```

```cpp
// domain/ports/IUpstreamThreadStore.h —— 上游会话映射
// 对应原 eraseChatinfoMap / transferThreadContext
class IUpstreamThreadStore {
public:
    virtual ~IUpstreamThreadStore() = default;
    virtual void unbind(std::string_view conversationId) = 0;
    virtual void rebind(std::string_view fromId, std::string_view toId) = 0;
};
```

**要点**：
- `generate` 接收 `const ProviderRequest&` 而非 `session_st&`；
  `ProviderRequest` 是为上游裁剪过的扁平 DTO，不含可变会话状态。
- `ProviderCapabilities` 用于替代当前散落各处的 `if (providerName == "xxx")` 硬判断。

---

### 4.2 ProviderBase 模板方法（解决 P6）

```cpp
// infrastructure/provider/ProviderBase.h
class ProviderBase : public IChatProvider {
public:
    ProviderBase(IAccountRepository& accounts, IMetricsSink& metrics, IClock& clock);

    // final：共性流程固定，子类不得覆盖
    [[nodiscard]] Result<Generation> generate(const ProviderRequest& req) final;

protected:
    // —— 差异点，子类必须实现 ——
    [[nodiscard]] virtual Result<HttpRequest>  buildRequest(const ProviderRequest&) = 0;
    [[nodiscard]] virtual Result<void>         applyAuth(HttpRequest&, const Account&) = 0;
    [[nodiscard]] virtual Result<ChunkOutcome> parseChunk(std::string_view raw) = 0;
    [[nodiscard]] virtual Error mapUpstreamError(int status, std::string_view body) = 0;

private:
    // —— 共性，且各自独立可单测 ——
    SseFramer            framer_;
    RetryPolicy          retry_;
    IAccountRepository&  accounts_;
    IMetricsSink&        metrics_;
    IClock&              clock_;
};
```

共性流程（基类内固定）：
账号选取 → 鉴权 → 发起请求 → 超时控制 → SSE 分帧 → 逐片解析
→ 错误映射 → 重试退避 → 指标上报 → 归一化返回。

> `parseChunk` 用 `std::string_view` 实现零拷贝解析——这是保留 C++17 的直接收益之一。

---

### 4.3 生成流水线（解决 P5）

将 `GenerationServiceEmitAndToolBridge.cpp` 的 2213 行拆为显式 stage：

```
NormalizeRequest → ResolveContinuity → ApplyHistoryBudget
→ EncodeToolDefinitions → CallProvider → DecodeToolCalls
→ ValidateToolCalls → NormalizeToolArgs → SanitizeOutput → Emit
```

```cpp
// application/pipeline/Stage.h
class Stage {
public:
    virtual ~Stage() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual Result<void> run(GenCtx& ctx) = 0;
};

class Pipeline {
public:
    template <typename S, typename... Args>
    Pipeline& emplace(Args&&... args) {          // 就地构造，避免调用方写 make_unique
        stages_.push_back(std::make_unique<S>(std::forward<Args>(args)...));
        return *this;
    }
    [[nodiscard]] Result<void> execute(GenCtx& ctx);   // 任一 stage 失败即短路
private:
    std::vector<std::unique_ptr<Stage>> stages_;
};
```

按能力动态裁剪：

```cpp
Pipeline p;
p.emplace<NormalizeRequest>()
 .emplace<ResolveContinuity>(continuityResolver)
 .emplace<ApplyHistoryBudget>(budget);

const auto caps = provider.capabilities();
if (!caps.nativeToolCalls) {           // 上游不支持原生工具调用才装桥接
    p.emplace<EncodeToolDefinitions>(encoder);
}
p.emplace<CallProvider>(provider);
if (!caps.nativeToolCalls) {
    p.emplace<DecodeToolCalls>(xmlCodec);
}
p.emplace<ValidateToolCalls>(validator)
 .emplace<NormalizeToolArgs>()
 .emplace<SanitizeOutput>()
 .emplace<Emit>(sink);
```

**收益**：每个 stage 可独立单测；日志与 tracing 统一在 `Pipeline::execute` 中植入。

---

### 4.4 组合根（解决 P2）

```cpp
// AppContext.h
struct AppContext {
    // —— infrastructure ——
    std::shared_ptr<IClock>                clock;
    std::shared_ptr<IAccountRepository>    accounts;
    std::shared_ptr<IChannelRegistry>      channels;
    std::shared_ptr<ISessionStore>         sessions;
    std::shared_ptr<IMetricsSink>          metrics;
    std::shared_ptr<IProviderFactory>      providers;

    // —— application ——
    std::shared_ptr<ChatCompletionUseCase> chat;
    std::shared_ptr<ResponsesUseCase>      responses;
    std::shared_ptr<AccountUseCase>        account;
    std::shared_ptr<ChannelUseCase>        channel;

    [[nodiscard]] static Result<AppContext> build(const Config& cfg);
};
```

Controller 通过构造函数持有 UseCase 引用，不再调用任何 `getInstance()`。

---

### 4.5 Result<T, Error>（C++17 实现）

用 `std::variant` 做判别式联合——**不需要**自建 optional/expected 垫片：

```cpp
// platform/Result.h
struct Error {
    enum class Code {
        Network, Auth, RateLimited, InvalidRequest,
        Timeout, ServiceUnavailable, Internal, Unknown
    };
    Code        code = Code::Unknown;
    std::string message;
    std::string upstreamCode;   // 上游原始错误码
    int         httpStatus = 0;
};

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T v)      : data_(std::move(v)) {}
    Result(Error e)  : data_(std::move(e)) {}

    [[nodiscard]] bool ok() const noexcept {
        return std::holds_alternative<T>(data_);
    }
    explicit operator bool() const noexcept { return ok(); }

    const T&     value() const& { return std::get<T>(data_); }
    T&&          value() &&     { return std::get<T>(std::move(data_)); }
    const Error& error() const  { return std::get<Error>(data_); }

    // 函数式组合，减少层层 if (!r.ok()) return r.error();
    template <typename F>
    auto andThen(F&& f) -> decltype(f(std::declval<const T&>())) {
        if (!ok()) return error();
        return f(value());
    }

private:
    std::variant<T, Error> data_;
};

// void 特化
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error e) : error_(std::move(e)) {}
    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    const Error& error() const { return *error_; }
private:
    std::optional<Error> error_;
};
```

**相比 C++14 方案的优势**（保留 C++17 的直接收益）：
- 真正的判别式联合，不同时占用 T 与 Error 的空间
- 不要求 `T` 可默认构造
- `[[nodiscard]]` 使忽略错误变为编译警告
- `andThen` 让 Pipeline 各 stage 的错误传播链条简洁

---

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

原方案把「159 用例全绿」当作每个阶段的默认门禁。但用例只能证明「它 include 的那份代码没坏」，
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

**分母校正**：`DROGON_TEST` 实测 **159** 个（24 个文件），非历史版本写的 160。

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

##### R2 实测：**23 个命中**（v2.0 重测；由 2 → 23 是口径错误，不是漏扫）

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
| **A：已链接** | **5** | `.cpp` 已在 `PROJECT_SOURCES`，只是没人写用例 | 新增 `test_*.cpp` + `TEST_SOURCES` 加一行，**零构建风险** |
| **B：未链接** | **18** | `.cpp` 不在 `PROJECT_SOURCES`，或为纯头文件组件 | 须先改 CMake 引入源文件，可能牵出新链接依赖，**逐条评估** |

> **排序原则：先清空 A 类，再动 B 类。** A 类每条边际成本相同且可预测；
> B 类每条都要单独评估链接闭包，**不可批量估算工期**。
> 其中 4 条为 `impl=0` 的纯头文件组件（`ToolDefinitionResolver` / `BackgroundTaskQueue` / `ProviderResult` / `Apicomn`），
> 需先判定「是否存在可断言行为」再决定是否属于 R2 的合理治理对象 —— 有可能是**规则误报**，待逐条核实。

##### R3 实测：12 个函数 / 8 个文件 / **4,725 行**

| 行数 | 文件 | 位置 |
|---:|------|------|
| **865** | `chaynsapi.cpp` | 283–1147 |
| **554** | god 文件 `transformRequestForToolBridge` | 1660–2213 |
| **503** | god 文件 `emitResultEvents` | 515–1017 |
| 459 | `retoolapi.cpp` | 847–1305 |
| 375 | `XmlTagToolCallCodec.cpp` | 678–1052 |
| 359 | `accountManager.cpp` | 1745–2103 |
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

> 本节是 R2 从 23 条开始下降的**第一批执行单**。目标选取只依据 0-E-1 的 R2 定义，不引入新尺子。

##### A 类 5 条的处置排序

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
| 一次性清空 R2 | 剩余 18 条为 B 类，须先改 CMake，属另一类工作 |

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

**门禁**：`grep -ri openai src/` 无输出；全量构建通过；159 用例全绿。

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
- 全量构建 + 159 用例通过

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

- `grep -rn "getInstance" src/` 结果为 **0**（删除类断言）
- 领域层单测可在无 DB / 无网络环境下运行
- **A 类新增**：进程收到 SIGTERM 后 5 秒内干净退出，ASan/TSan 无 use-after-free 报告
- 每批次结束全量构建 + 159 用例通过

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
| 1 骨架地基 | 1.0 周 | 3.0 | 五层 target + include 收敛 + Result |
| 2 消灭单例 | 1.5 周 | 4.5 | AppContext，`getInstance` 归零 |
| 3 Provider 归一 | **1.0 周** | 5.5 | 接口收窄，仅需协调 2 家实现（原 1.5 周 / 4 家） |
| 4 拆上帝对象 | 1.5 周 | 7.0 | 生成流水线 + 账号组件（accountManager 已瘦身 31%） |
| 5 收口 | 0.5 周 | 7.5 | 错误模型统一、构建优化 |

**表内合计 7.5 周** —— 但这是**阶段基线**，不含 v1.7 之后新增的专项工作。

##### v2.1 工期对账（此前 §9 与 §10 互相矛盾，本节为唯一口径）

| 来源 | 增量 | 说明 |
|---|---:|---|
| §9 阶段基线 | **7.5 周** | 上表七个阶段之和 |
| v1.7 新增 0-E 测试有效性审计 | **+1 天** | 已发生 |
| v1.8 净增（`transformRequestForToolBridge` 特性化 +1.5 天，审计 −0.5 天，阶段 4 转发壳 −0.5 天） | **+0.5 天** | 已发生 |
| **对账后总工期** | **≈ 7.9 周** | **以此为准** |
| §0-E-2 A 类 5 条（P0–P4） | **未计入** | 见下表 |
| R2 B 类 18 条 | **不可估算** | 见 §7 风险表 |

> **v2.1 纠正**：§9 这张表自 v1.6 起未随变更记录更新，长期显示 7.5 周，而 §10 的 v1.8 条目写的是 7.9 周，
> 二者矛盾持续了三个版本。**以 7.9 周为准**；上表保留 7.5 是因为它确实是「阶段基线」，删掉会丢失分解结构。

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

## 0-E-3. R2 分类法修正：A / H / B 三分（v2.2）

### 推翻 v2.0 自己定的 A/B 二分法

v2.0 把 23 条分为「A 已链接(5)」与「B 未链接(18)」，判据是 `.cpp` 是否在 `PROJECT_SOURCES`。
**该判据对纯头组件是错的** —— header-only 组件根本没有 `.cpp` 可链接，测试它只需 `#include`，**CMake 一行都不用改**。
把它们归入「须先改 CMake」的 B 类，等于凭空记了 4 笔不存在的成本。

### 修正后的三分法

| 类别 | 数量 | 判据 | CMake 成本 |
|---|---:|---|---|
| **A 已链接** | **5** | `.cpp` 已在 `PROJECT_SOURCES` | `TEST_SOURCES` 加一行 |
| **H 纯头（新增）** | **3** | `impl=0` 且头内有 inline 函数体 | **零** |
| **B 未链接** | **14** | 有 `.cpp` 但不在 `PROJECT_SOURCES` | 须改 CMake + 评估链接闭包 |
| **排除** | **1** | 无可断言行为 | 不适用 |
| 合计 | **23** | | |

### H 类 3 条逐条结论

| 组件 | 头行数 | inline 函数体 | 结论 | 排期 |
|---|---:|---:|---|---|
| `tooling/ToolDefinitionResolver.h` | 176 | 11 | **真实缺口，价值最高**。`namespace toolcall` 下的自由函数，含 `visitToolDefinitionsImpl` 对嵌套 namespace 工具定义的**递归下降遍历**（`declaredType == "namespace"` 分支、`_aiapi_namespace` 元数据回读）。纯函数、无全局状态 | **H1 · 1 天** |
| `apipoint/ProviderResult.h` | 152 | 12 | **真实缺口，成本极低**。`isSuccess()` / `hasError()` / `isValid()` 三个判定式 + 5 个静态工厂。`fail()` 内 `err.httpStatusCode > 0 ? … : 500` 是唯一有真实分支的工厂 | **H2 · 0.5 天** |
| `utils/BackgroundTaskQueue.h` | 152 | 14 | **真实但不属本阶段**。函数内静态单例 `static BackgroundTaskQueue& instance()` + `std::vector<std::thread> workers_` 线程池。测它需控制线程调度与 `shutdown()` 时序，**与阶段 2「消灭单例」是同一件事**，此处单独补测会做两遍 | **不排期，转阶段 2** |

### 排除项：`Apicomn.h`（真误报）

全文 13 行：`struct session_st;` / `class APIinterface;` 两个前向声明，加一个只有单个枚举值的 `enum class ApiChannel`。
**无函数、无数据成员、无任何可断言行为** —— 为它写测试只能断言「枚举值等于自己」，正属 §0-E 定义的空转用例。

> **规则修正建议（尚未执行）**：R2 应增加排除条件「`impl=0` 且 inline 函数体数 = 0」。
> **本版故意未改脚本、未改 `audit-baseline.json`（仍为 23）** —— 改规则会同时改动基线，
> 必须单独一个 commit 并重跑 `--selftest`，混在文档提交里会让防回归失去意义。
> 当前状态：**文档口径 22 条真实缺口 + 1 条待排除；脚本口径 23 条**，差异原因即此。

### B 类 14 条分层（替代 v2.0 的扁平列表）

| 层 | 条数 | impl 行数 | 处置原则 |
|---|---:|---|---|
| **B1 小** | **8** | 30–181 | 可逐条立项，单条 0.5–1 天。扇入最高的 `channelManager.h`(5/175)、`RetoolWorkspaceManager.h`(5/73) 优先 |
| **B2 中** | **3** | 383–452 | 三条全是 `dbManager`（chaynsThread / retoolWorkspace / channel）。**须先定 DB 替身策略**，与 §0-E-2 的 P4 同一前置 |
| **B3 大** | **3** | 532–2601 | `accountManager.h`(2601) / `chaynsapi.h`(1441) / `GenerationService.h`(532)。**禁止估时**；前两条已在 §7 风险表单列，须先函数内拆分 |

> **B2 与 P4 合并前置**：`SessionDbManager`（P4，490 行）与 B2 三条面对同一个问题 —— 真实 DB + 异步队列。
> 一次性定出 DB 替身策略可同时解锁 4 条，比逐条摸索更省。**但该策略尚未设计，故这 4 条一律不估时。**

---

## 10. 变更记录

| 版本 | 日期 | 变更 |
|------|------|------|
| v1.0 | 2026-08-07 | 初稿，基于 C++20 假设 |
| v1.1 | 2026-08-07 | 按初步决策改为 C++14；补充 C++14 替代方案与自建垫片设计 |
| v1.2 | 2026-08-07 | **实测确认项目现状为 C++17，标准定为 C++17**。回退 v1.1 的降级设计：删除 Optional/StringView 垫片，`Result<T>` 改用 `std::variant` + `[[nodiscard]]`，Pipeline 恢复模板 `emplace`。新增问题项 **P9（标准探测漂移 + 主程序 17 与测试 20 不一致）**及其对策；阶段 1 恢复为 1 周，总工期回到 7 周 |
| v1.3 | 2026-08-07 | **P9 已修复并落地**（commit `efb4003`：硬编码 C++17、移除探测降级、测试目标 20→17、120+57 编译单元实测一致、160 用例全绿），新增 §0.4b。新增 **§0.5 测试覆盖基线实测**：模块覆盖 41.0%，五个改动最大的文件（accountManager / EmitAndToolBridge / chayns·retool·nexos api，合计 8,939 行）**零覆盖**，确认为当前最大单点风险。**核查 `har/` 后判定其不可用**（5 文件 297 条目均为前端浏览器抓包，无本服务 SSE 流量），废止「用 har 构造黄金响应」方案，改为三层安全网（契约快照 / 特性化 / 端到端冒烟），阶段 0 由 1.0 周上调至 1.5 周，总工期 7 → 7.5 周 |
| v1.4 | 2026-08-07 | **纠正 v1.3 对 `har/` 的误判**。确认当前唯一活跃上游为 chayns（retool / nexos / genspark 为历史遗留），且 chayns 上游**走轮询而非 SSE**（`chaynsPollingPolicy.h`：5 min 截止 + 退避），v1.3 按 `text/event-stream` 检索故全部落空；har 的 WS 帧存于 `_webSocketMessages` 字段亦被漏读（77 帧，但代码侧不使用 WS）。复核后 `chaynsapi.cpp` 的 6 类端点在 har 中**全部命中且含完整请求/响应体**，20 个端点可直接落 fixture。阶段 0 改为四层安全网并以真实 har 为 L1 数据源；retool/nexos/genspark 降级为「仅保证编译与不崩」；新增 fixture 字段归一约定（为可复现性，非安全兜底）。覆盖率门槛由 ≥65% 调整为 ≥60%（不再要求覆盖非活跃 Provider）。工期维持 7.5 周 |
| v1.5 | 2026-08-07 | **Provider 范围决策：仅保留 chayns + retool**，新增 **§6 阶段 0.5「Provider 下线」**（0.5 周），前置于所有结构性改造。实测发现 `openai` 为已编译已注册但**无路由不可触达**的死代码（新增 P10），`/nexosapi/*` 路由实际复用 chayns handler 导致命名误导（新增 P11）。nexos 删除清单共 14 处：5 处整体删除 + 9 文件外科手术，其中 `accountManager.cpp` 含 78 处引用 / 9 个专属函数。**P6 结论修正**：实测三家 Provider 私有方法并不同构，「同构约 60%」不成立，阶段 3 目标由「归一到公共基类」改为**「归一到瘦接口」**（复用已存在的 `contracts/GenerationRequest.h`），工期 1.5 → 1.0 周。总工期维持 7.5 周（0.5 周新增成本由阶段 3 缩短完全抵消）。新增不可回滚风险项：nexos 账号数据需在代码删除前清理 |
| v1.6 | 2026-08-07 | **阶段 2 细化并重排为 5 批次**。实测工作面：21 个单例类 / 约 180 处调用，前 6 类占 60%。**撤销 P2 中「chatSession 双实例」的判断**（新增 P2b 条目说明）：`Session.h:156` 的裸指针成员定义为 `nullptr` 后从无读写，被 `getInstance()` 内同名局部 static 遮蔽，实为死代码而非并发隐患。**ADR-06 两处修正**：(1) `main.cc:235-241` 已是显式有序的组合根雏形，AppContext 构建难度被高估；(2) 单例按状态性质分 A/B/C 三类，原「全部注入」一刀切不成立。**新增高风险项**：`chatSession` 持有 `std::thread` 成员，注入化改变析构时序，需先补 `shutdown()` 再改所有权，并增设 ASan/TSan 门禁。A 类内部顺序强约束 `AccountManager` 排最后（等阶段 0.5 瘦身 31% 后再动）。工期维持 1.5 周，风险集中至 2-e 批次，前四批可独立发布 |
| v1.7 | 2026-08-07 | **阶段 4 踏点，发现 P12：测试与生产路径脱钩**。`normalizeToolCallArguments` / `generateForcedToolCall` 各有两份独立实现：生产走 `GenerationService::` 静态成员（258 / 234 行，功能完整），测试走 `toolcall::` 自由函数（41 / 51 行，功能残废）；调用点无命名空间限定且位于类作用域内，名字查找永远命中成员版 → **10 个用例全空转**。**阶段 0 新增 0-E 「测试有效性审计」**（1 天），「160 用例全绿」不再单独构成验收条件，必须附带「空转用例数 = 0」。**阶段 4 方向修正**：权威实现是 god 文件那份，应「搬 god 版去覆盖 tooling 残废版」而非反向切换（反向会使 `tool_choice` 定向调用全部失效）。**补体量基线**：god 文件实为 2,213 行且路径在 `core/` 而非 `generation/`；新增发现 `XmlTagToolCallCodec.cpp`（1,314 行）同样超标但原方案漏列。风险表新增两项高危。阶段 0 工期 +1 天，总工期 7.5 → 7.7 周 |
| v1.8 | 2026-08-07 | **待查清单结案 + 三处对 v1.7 的校正**。（1）**`applyStrictClientRules` 误判纠正**：v1.7 称其为「唯一切换成功案例」有误，实为成员版 6 行**转发壳**委派给 200 行组件版；（2）**脱钩函数 2 → 4**：全库 47 个导出自由函数扫描后新增 `transformRequestForToolBridge`（成员 554 行 vs 组件 29 行，**无任何测试**）；（3）**用例数 160 → 159**（`DROGON_TEST` 实测，24 个文件）。**待查两个组件结果**：`ToolDefinitionEncoder`（29 行）确认为空壳，成员版多承担能力 IR 推导、协议格式决策、Codex systemPrompt 清空、definition_mode 开关；`ActionProtocolAdapter`（75 行）**健康非空壳**，由此得出「不得以行数小判定空壳」规则。**搬迁方案改为转发壳模式**（搬函数体 → 成员缩为转发 → 调用点零改动），优于 v1.7 的四步切换。**新增日志实证小节**（827 行 / 7 小时）：chayns 恒走文本桥、`accountManager` 日志居首 213 条、严格客户端路径零触发、`namespaceToolBridgeEnabled=0` 可降优先级、nexos 仍在校验账号（印证删除前必须清数据）。**工期**：审计 1 → 0.5 天；新增 `transformRequestForToolBridge` 特性化 +1.5 天；阶段 4 因转发壳模式 -0.5 天。总工期 7.7 → **7.9 周** |
| v1.9 | 2026-08-07 | **先固化尺子，再谈拆分（B 先于 A）**。**新增 §0-E-1「三条审计规则」**作为全阶段唯一判定依据：**R1 同名竞争**（实测 4 个，与 v1.8 一致，已收敛）、**R2 高扇入零测试**（*实测 2 个 —— 该数字已由 v2.0 纠正为 **23**，原因见 §0-E-1*，**新发现 `BridgeHelpers` 188 行 / 扇入 5 为全库最高 / 测试 0**，因无同名成员竞争而被 R1 漏扫）、**R3 函数级 >200 行**（实测 **12 个函数 / 8 个文件 / 4,725 行**）。配套硬规则：行数必须来自 `wc -l`，禁止目测。**两处数据校正**：`ToolDefinitionEncoder` 26 → **29** 行（v1.8 目测错误）；`transformRequestForToolBridge` ~550 → **554** 行（精确边界 1660–2213），god 文件 9 函数完整切分表入库。**两处前提推翻**：（1）「god 文件是最大单点」不成立 —— `accountManager.cpp` **2,600 行**，大 387 行且同样零覆盖、日志量居首；（2）全库最大单函数不在 god 文件，而是 `chaynsapi.cpp` 的 **865 行**（283–1147，零覆盖）。**补齐漏列范围**：`RequestAdapters.cpp`（1,255 行，日志第 4，36 条）与 `Session.cpp`（1,221 行）合计 2,476 行活跃主路径代码。**拆分语义拆为两个动作**：A 函数内拆分（必须先做）/ B 文件拆分（依赖 A）；god 文件 4 个超标函数共 1,549 行占 70%，先做 B 属假性完成。审计脚本重命名为 `tools/architecture_audit.py`。风险表新增三项。**本版不调总工期** —— 阶段边界重划需待 A 阶段（`RequestAdapters` / `Session` / `accountManager` 摸底）完成后统一重估 |
| v2.0 | 2026-08-07 | **R2 口径纠错 + 首批处置单落地**。（1）**R2 由 2 → 23，是口径错误不是漏扫**：v1.9 用头文件 basename 子串匹配判断是否被测，既误判也漏扫；`tools/architecture_audit.py` 改用 **`g++ -MM` 编译期依赖闭包**，并以 `src/test/CMakeLists.txt` 的 `TEST_SOURCES` / `PROJECT_SOURCES` 为真值来源，同时修正测试信号识别（`DROGON_TEST` / `CHECK` / `REQUIRE`，此前误按 gtest 假定）。基线落盘 `doc/adr/audit-baseline.json`，错误版本留档 `audit-baseline.INVALID-r2-bug.json.bak`。（2）**`BridgeHelpers` 已补测出榜**（`src/test/test_bridge_helpers.cpp`）。（3）**新增 §0-E-2「R2 首批处置方案」**：23 条按「是否已链接进测试二进制」分 **A 类 5 条 / B 类 18 条**，A 类优先（零构建风险），排序 P0 `SessionCodec` → P1 `TextExtractor` → P2 `ApiManager` → P3 `ApiFactory` → P4 `SessionDbManager`。（4）**新增 P13**：`ApiManager` 四项真实缺陷 —— `getApiInfoByModelName` 空队列 `top()` 为 **UB**、查询接口用 `operator[]` 致 nullptr 条目膨胀、`updateApiInfo` 空实现、`flushModelnameApiQueueMap` 疑似空操作。（5）**三处自我推翻**：`app()` 在测试中**可用**（`test_main.cc` 于后台线程跑事件循环），故排除 `init()` 的理由改为「无信息量 + 单例污染」而非「会崩」；`session_st` 为 **32** 字段而非 31；`apiType` 映射为 **3 份**而非 2 份。（6）**三项探查结论**：`ApiType` 枚举确认仅 2 值（三元映射完备）；`Session.h` 中 OpenSSL 仅见于注释与声明、无内联实现，**P0 无链接风险**；`v` 字段全仓仅 1 处写入、**零处读取**，为死字段。（7）**变异验真由 1 条增至 4 条** —— 用例间覆盖重叠，单条不足以证明断言独立有效。（8）**新增待核实项**：R2 中 4 条 `impl=0` 的纯头文件组件可能是**规则误报**，需逐条判定是否存在可断言行为。**本版不调总工期**；R2 降幅、用例数与断言数均须实跑后回填，不做估算 |
| v2.1 | 2026-08-07 | **消歧义版：把「不明确」全部落成可复算的确定值或显式标注**。（1）**修正三个版本的工期矛盾**：§9 长期显示 7.5 周、§10 的 v1.8 写 7.9 周，新增「工期对账」表锁定 **7.9 周为唯一口径**，并说明 7.5 是阶段基线故予保留。（2）**§0-E-2 首次获得工期归属**：P0 2 天 / P1 0.5 天 / P2 1.5 天 / P3 0.5 天 = **4.5 天 ≈ 0.9 周**，计入后 **≈ 8.8 周**；P4 明确**暂不排期**；P2 估时标注为**唯一带前置条件项**（`ProviderResult` 可否默认构造，不成立则作废转入不可估算区）。（3）**§7 风险表补 3 行**：P13 的 UB（高，标注为当前唯一已定位的生产期 UB）、R2 B 类 18 条链接闭包不可预测（高，对策为**禁止打包报工期**）、4 条纯头组件疑似规则误报（中，若成立应修规则而非硬补测试）。（4）**两处数字补出处**：`ImageInfo` 5 字段实为 `contracts/GenerationRequest.h:110` 定义而非 `Session.h`；`SessionCodec.cpp` 四段变量名 `req`/`resp`/`st`/`pv` 与 10+4+11+7=32 已可复算。（5）**阶段 1/3/5 显式标注「有意暂缓细化」**并给出细化触发条件，消除「粗纲 = 遗漏」的歧义。本版**只消歧义，不改任何技术决策** |
| v2.2 | 2026-08-07 | **三项待定全部实证结案，并推翻自己的 A/B 二分法**。（1）**议题 1 结案**：`provider::ProviderResult` 经四条 `static_assert` + `g++ -fsyntax-only` 实证为**可默认构造/拷贝/移动/赋值（rc=0）**，系纯聚合体、7 个成员全带默认初值。P2 的 1.5 天**解除前置条件**，至此 §9 估时表**不含任何待定条件**。（2）**议题 3 结案**：4 条 `impl=0` 组件逐条实证 —— 仅 `Apicomn.h`（13 行，2 前向声明 + 单值 enum）为**真误报**；`ToolDefinitionResolver`（含 namespace 递归遍历逻辑）与 `ProviderResult`（3 判定式 + 5 工厂）是**真实缺口**；`BackgroundTaskQueue`（函数内静态单例 + 线程池）真实但**并入阶段 2 单例治理**，避免做两遍。（3）**议题 2 结案 + 自我推翻**：新增 **§0-E-3**，v2.0 的 A/B 二分法**判据有误** —— 纯头组件无 `.cpp` 可链接、测试零 CMake 成本，却被归入 B 类，凭空多记 4 笔成本。改为 **A(5) / H(3) / B(14) / 排除(1)** 三分法；B 类由 18 收窄至 14 并分 B1(8)/B2(3)/B3(3) 三层，B2 三条 dbManager 与 P4 合并为同一前置（DB 替身策略未定，4 条一律不估时），B3 三条仍禁止估时。（4）**工期**：H 类 2 条 +1.5 天 ≈ 0.3 周，总计 **≈ 9.1 周**（7.9 基线 + 0.9 A 类 + 0.3 H 类）。（5）**故意未做**：R2 排除规则未写入脚本、`audit-baseline.json` 仍为 23 —— 改规则须同时改基线，混入文档提交会让防回归失效；文档口径(22+1)与脚本口径(23)的差异已在 §0-E-3 记录 |
