# RFC-001 aiapi 架构重构方案

| 项目 | 内容 |
|------|------|
| 编号 | RFC-001 |
| 状态 | 草案 (Draft) |
| 版本 | v1.2 |
| 日期 | 2026-08-07 |
| 范围 | `src/` 全量约 39,000 行 C++ |
| 语言标准 | **C++17**（与现状一致，见 ADR-04） |

---

## 0. 背景与问题陈述

项目由增量迭代而成，当前 14 个模块 / 约 39K 行 / 20+ 单例。

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
| P2 | 全局可变状态泛滥 | 20+ 处 `getInstance()`；`chatSession` 同时存在裸指针 static 与函数内 static 两份实例 |
| P3 | 依赖方向倒挂 | `APIinterface::generate(session_st&)` 使基础设施层直接修改领域对象 |
| P4 | 接口职责混杂 | `APIinterface` 同时承担生成 / 生命周期 / 上游会话状态三类职责 |
| P5 | 上帝对象 | `accountManager.cpp` 2600 行、`GenerationServiceEmitAndToolBridge.cpp` 2213 行 |
| P6 | Provider 大量复制 | chayns 1440 + retool 1352 + nexos 1334 = 4126 行，同构逻辑约占 60% |
| P7 | 构建耦合 | 单一 `add_executable`，无模块边界；`Session.h` 被 12 处包含，改动触发大范围重编 |
| P8 | 上轮重构未收口 | `ProviderResult` 已定义，但调用链仍走 session 副作用；`plans/aiapi-refactor-design.md` 已不存在 |
| **P9** | **语言标准由探测决定，存在漂移风险** | 见 §0.4，主程序与测试目标可能编译在不同标准下 |

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
| `getInstance()` 出现次数（业务代码） | 20+ | 0 |
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
│   │   ├── chayns/  nexos/  retool/  openai/
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

### 阶段 0 · 安全网（1 周）

| 任务 | 产出 |
|------|------|
| 用 `har/` 现有抓包构造回归 fixture | 每 Provider × {流式, 非流式} × {有, 无工具调用} |
| 建立黄金响应比对测试 | 重构前后响应逐字节一致 |
| 记录基线指标 | 全量 / 增量构建时长、二进制体积、P99 延迟 |
| 修复 `chatSession` 双实例隐患 | 消除 `static chatSession* instance` 裸指针 |
| **修复标准漂移（P9）** | 移除探测逻辑；主程序与 test 统一 17；验证 Docker 内亦为 17 |

**门禁**：黄金用例全绿且可重复执行；`-std=` 参数在主程序与测试中一致。

---

### 阶段 1 · 骨架与地基（1 周）

- 建立五个 library target 与目录骨架（先空壳，后续逐步搬迁）
- 收敛 include 路径为单一根，批量重写现有 `#include`（脚本可完成大部分）
- 引入 `platform/Result.h`（`std::variant` 实现 + `[[nodiscard]]`）

> 因保持 C++17，**无需任何特性迁移工作**，本阶段维持 1 周。

**门禁**：
- 构建通过
- 黄金用例全绿
- CI 加入「domain target 不得链接 Drogon」检查

---

### 阶段 2 · 消灭单例（1.5 周）

迁移顺序（依赖少的先做）：

```
IClock → IMetricsSink → 7 个 DbManager → ChannelManager
→ AccountManager → SessionExecutionGate → ResponseIndex
→ chatSession → ApiFactory / ApiManager
```

每个对象的标准动作：
定义 port → infrastructure 实现 → AppContext 注册 → 改完全部调用点 → **删除 `getInstance()`**

**门禁**：
- `grep -rn "getInstance" src/` 结果为 0
- 领域层单测可在无 DB / 无网络环境下运行

---

### 阶段 3 · Provider 归一（1.5 周）

1. 抽出 `SseFramer` / `RetryPolicy` 并补齐单测
2. 落地 `ProviderBase`
3. 逐个迁移：**openai**（最简，用于验证设计）→ **nexos** → **retool** → **chayns**（最复杂，含线程回收）
4. 落地 `ProviderCapabilities`，删除散落的 provider 名称硬判断
5. **收口 P8**：彻底移除 `generate(session_st&)` 的 session 副作用路径

**门禁**：
- 单 Provider < 400 行
- 新增一个 mock provider，验证 200 行内可完成接入

---

### 阶段 4 · 拆解上帝对象（1.5 周）

**4a. `GenerationServiceEmitAndToolBridge.cpp` (2213 行) → 流水线**
逐 stage 抽出，每抽一个跑一次黄金用例。

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
| 重构期间行为漂移 | 高 | 阶段 0 黄金用例；重构 commit 禁止夹带功能变更 |
| 又一次「只加不删」半途而废 | 高 | 每阶段门禁均为**删除类断言**（grep 计数、行数上限） |
| chayns 上游线程语义复杂，迁移易错 | 高 | 放在 Provider 迁移最后；先用前三个验证 ProviderBase 设计 |
| 标准漂移（换编译器后变 C++20 / 测试用 20） | 中 | ADR-04 钉死 17 + `target_compile_features` + CI 校验 `-std=` |
| include 批量重写引入编译错误 | 中 | 脚本改写 + 全量构建验证，单独一个 commit 便于回滚 |
| 长命分支导致合并地狱 | 中 | Strangler Fig，每日合主干，单阶段不超过 1.5 周 |
| 提交历史不可追溯 | 中 | 立即改用规范 commit message（现状多为 `08070939` 类时间戳） |

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
   - 黄金用例必须通过
4. **每阶段结束打 tag**，便于回滚与对比基线指标

---

## 9. 时间线汇总

| 阶段 | 周期 | 累计 | 核心交付 |
|------|-----:|-----:|----------|
| 0 安全网 | 1.0 周 | 1.0 | 黄金用例 + 基线指标 + 标准固定 |
| 1 骨架地基 | 1.0 周 | 2.0 | 五层 target + include 收敛 + Result |
| 2 消灭单例 | 1.5 周 | 3.5 | AppContext，`getInstance` 归零 |
| 3 Provider 归一 | 1.5 周 | 5.0 | ProviderBase，单 Provider < 400 行 |
| 4 拆上帝对象 | 1.5 周 | 6.5 | 生成流水线 + 账号五组件 |
| 5 收口 | 0.5 周 | 7.0 | 错误模型统一、构建优化 |

**合计约 7 周，全程主干可发布。**

> **最小可行子集**：若资源受限，阶段 0 + 1 + 2（3.5 周）即可获得约 80% 收益
> —— 依赖可见、领域可测、单例清零。阶段 3~5 可按需延后。

---

## 10. 变更记录

| 版本 | 日期 | 变更 |
|------|------|------|
| v1.0 | 2026-08-07 | 初稿，基于 C++20 假设 |
| v1.1 | 2026-08-07 | 按初步决策改为 C++14；补充 C++14 替代方案与自建垫片设计 |
| v1.2 | 2026-08-07 | **实测确认项目现状为 C++17，标准定为 C++17**。回退 v1.1 的降级设计：删除 Optional/StringView 垫片，`Result<T>` 改用 `std::variant` + `[[nodiscard]]`，Pipeline 恢复模板 `emplace`。新增问题项 **P9（标准探测漂移 + 主程序 17 与测试 20 不一致）**及其对策；阶段 1 恢复为 1 周，总工期回到 7 周 |
