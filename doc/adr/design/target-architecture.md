# 目标代码结构与依赖组织

> 本文定义重构完成后必须保持的**物理文件树、target DAG 和对象所有权**。
> 文件名示例是当前 canonical tree 的职责索引，而不是要求一次性创建所有未来类。
> 历史目录和旧路径的事实记录留在审计/工作产物；当前施工状态见
> [`migration-plan.md`](../plans/migration-plan.md)。

## 1. 顶层依赖图

箭头指向被依赖 target。P8-W1 已删除 `aiapi_legacy`；P8-W2 已让磁盘上的
`src/` 树也只使用这些正式层，而不是仅让 CMake owner 看起来正确。

```text
aiapi_application ──> aiapi_domain ──> aiapi_platform
aiapi_infrastructure ─┬─> aiapi_domain
                       └─> aiapi_platform
aiapi_transport ──────┬─> aiapi_application
                       ├─> aiapi_domain
                       └─> aiapi_platform
aiapi_runtime ────────┬─> aiapi_application
                       ├─> aiapi_infrastructure
                       └─> aiapi_transport
aiapi (main.cc) ──────> aiapi_runtime
```

实际 CMake DAG（`aiapi_transport` 还以 whole-archive 形式进入最终可执行文件，以保留
Drogon 的静态 Controller 注册对象）为：

```text
aiapi_platform
└── aiapi_domain
    └── aiapi_application

aiapi_infrastructure ──> aiapi_domain, aiapi_platform, Drogon/OpenSSL/PostgreSQL
aiapi_transport      ──> aiapi_application, aiapi_domain, aiapi_platform, Drogon
aiapi_runtime        ──> aiapi_application, aiapi_infrastructure, aiapi_transport
aiapi (main.cc)      ──> aiapi_runtime (+ whole-archive aiapi_transport)
```

约束：

- `domain` 只依赖标准库和 `aiapi_platform`；禁止 JsonCpp、Drogon、DB、OpenSSL。
- `application` 通过 domain port 编排；可在 request/event 值边界使用 JsonCpp，但禁止 include
  concrete Provider、persistence adapter 或 Drogon。
- `infrastructure` 实现 port，可依赖第三方库；不得被 domain/application 反向 include。
- `transport` 只做 HTTP/SSE/JSON 映射和 use-case 调度，不持有业务状态。
- `runtime` 是唯一组合根，负责构造、注入、启动和停机。

## 2. Canonical physical tree

`src/` 顶层目录是正式层边界，不再以历史业务目录充当第一层。除根文件
`CMakeLists.txt` 与 `main.cc` 外，当前允许的顶层目录**恰好**为：
`application/`、`domain/`、`infrastructure/`、`platform/`、`runtime/`、`test/`、`transport/`。

```text
src/
├── platform/
│   ├── result/{Result,Error,ErrorCode}.h
│   └── {Base64,Cancellation,Deadline,LocalDateTime,Log,ThreadJoin,Uuid,ZeroWidthEncoder}.{h,cpp}
├── domain/
│   ├── model/                 值对象、Provider request/response、指标和 session 数据
│   ├── policy/                纯业务策略
│   └── port/                  Store、Provider、clock、use-case、sink 等接口
├── application/
│   ├── account/               账号选择、注册、token、health、worker、admin use case
│   ├── channel/               渠道管理 use case 与 facade
│   ├── generation/
│   │   ├── actionProtocol/    能力识别与协议编译
│   │   ├── continuity/        ResponseIndex、连续性和文本提取
│   │   ├── contracts/         Generation request/event/sink 契约
│   │   ├── core/              AiApiUseCase、generation pipeline、session facade
│   │   └── tooling/           tool bridge、codec、validator、normalizer
│   ├── health/                readiness use case
│   ├── metrics/               metrics use case
│   └── workspace/             workspace use case 与 application facade
├── infrastructure/
│   ├── account/               Drogon Account HTTP adapter 与 concrete clocks
│   ├── codec/                 infrastructure-owned Retool JSON codec
│   ├── config/                ConfigValidator
│   ├── executor/              BackgroundTaskQueue
│   ├── managedAccount/        backend、contract 和 service adapter
│   ├── metrics/               ErrorStats config/service 与 decoder
│   ├── persistence/           account/channel/config/session/metrics/workspace/thread DB adapters
│   ├── provider/
│   │   ├── chayns/            Chayns provider、HTTP、clock、catalog、reaper
│   │   ├── limits/            history/outbound budget
│   │   └── retool/            Retool provider、HTTP 与 clock
│   └── workspace/             RetoolWorkspaceService
├── transport/
│   └── controllers/
│       ├── codecs/            HTTP request/response JSON codec
│       ├── sinks/             JSON/SSE sinks 和 event-loop response adapter
│       └── *.h/.cc            Drogon Controller、filter、tombstone、utility
├── runtime/                   AppContext、AppWiring、StartupResult
├── test/                      CTest unit、fixture、support 和 stub
├── CMakeLists.txt
└── main.cc
```

P8-W2 的搬迁映射是：

| 历史顶层职责 | Canonical 位置 |
|---|---|
| `accountManager/` | `application/account/`；clock/HTTP concrete adapter 在 `infrastructure/account/`，port 在 `domain/port/` |
| `channelManager/` | `application/channel/` |
| `sessionManager/` | `application/generation/{actionProtocol,continuity,contracts,core,tooling}/`；provider budgets 在 `infrastructure/provider/limits/` |
| `retoolWorkspace/` | application facade/use case 在 `application/workspace/`；service/codec/persistence adapter 在 `infrastructure/` |
| `dbManager/` | `infrastructure/persistence/` |
| `metrics/` | `infrastructure/metrics/` |
| `managedAccount/` | `infrastructure/managedAccount/` |
| `controllers/` | `transport/controllers/`（含 `codecs/` 与 `sinks/`） |
| `apiManager/`、`models/` | 已删除；Drogon model 描述移至仓库工具目录 `tools/drogon/` |

历史路径不得作为新 include、CMake source entry 或新文件落点复活。

## 3. 对象所有权

`AppContext` 持有所有有状态对象：`BackgroundTaskQueue`、session/response index、各 DB store、
`AccountManager`、workspace/managed-account service、`ProviderRegistry`、`ErrorStatsService` 与
`chaynsThreadReaper`。Controller、Provider 和 codec 均为构造注入的借用引用或 `shared_ptr`，不得自行创建
全局对象。

停机顺序固定为：停止接收 → 广播取消 → 停止 timer/reaper → executor Draining → 等待统一 deadline
→ join → 关闭 DB/HTTP。

## 4. 可执行的目录不变量

- `src/CMakeLists.txt` 的 `AIAPI_*_SOURCES` 中，每个 `.cpp/.cc` 必须在其正式 target 对应的
  物理层目录下；`domain` 维持 header-only。
- `tools/arch/check_physical_layout.py` 拒绝未知/历史顶层目录、错误物理层的 CMake source 和缺失源；
  `--selftest` 以内存错误层 source 证明该 gate 有杀伤力。
- `check_source_ownership.py --require-no-legacy` 负责 owner 唯一性，
  `check_target_layers.py --require-no-legacy` 负责 target DAG，`layer-rules.json` 负责 include 方向；
  三者不能互相替代。
- 自有头一律从唯一 `src/` 根使用完整路径 include；物理位置不构成绕过 ADR-01/02/09 的理由。
