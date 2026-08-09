# P3-W3 · 正式分层 target carve-out

| 项 | 值 |
|---|---|
| 状态 | DONE |
| 前置 | P3-W1 唯一 source owner；P3-W2 单一 include 根 |
| 目标 | 建立六个正式 target、迁入合法闭包、以机器 ceiling 约束剩余脚手架 |
| 决策 | [ADR-01](../decisions/ADR-01-layered-architecture.md)、[ADR-02](../decisions/ADR-02-cmake-enforced-layering.md)、[ADR-11](../decisions/ADR-11-production-test-targets.md) |

## 1. 目标 target DAG

```text
aiapi_platform

aiapi_domain
  └─> aiapi_platform

aiapi_application
  └─> aiapi_domain, aiapi_platform

aiapi_infrastructure
  └─> aiapi_domain, aiapi_platform, third-party IO

aiapi_transport
  └─> aiapi_application, aiapi_domain, aiapi_platform, Drogon

aiapi_runtime
  └─> aiapi_application, aiapi_infrastructure, aiapi_transport

aiapi (main.cc)
  └─> aiapi_runtime
```

图中箭头表示“左侧可依赖右侧”，不是线性层号。infrastructure 不得依赖
application；transport 不得直接访问 DB/Provider 具体实现；runtime 只组装、不实现业务规则。

## 2. 执行原则

1. 先从 `AIAPI_LEGACY_SOURCES` 和 include graph 生成每个 `.cpp/.cc` 的候选 owner；
2. 每次只 carve out 一个可编译闭包，同一提交中从 legacy 删除，禁止双 owner；
3. 每个正式 source list 使用 `AIAPI_*_SOURCES`，继续由 P3-W1 门禁检查；
4. 每个 target 只传播 P3-W2 的单一 `src/` include 根，禁止用子目录 include path 修补边界；
5. 若闭包需要上层具体类，先记录并提取窄 port，不制造反向 target link；
6. production 静态注册和 test stub 的 whole-archive/普通 archive 差异必须保真；
7. 每个闭包都运行 configure/build、定向测试、全量测试、source/include/layer/cycle 门禁和 link 证据。

## 3. 计划中的 carve-out 顺序

| 顺序 | Target | 初始闭包选择 | 状态 |
|---:|---|---|---|
| 1 | `aiapi_platform` | `ZeroWidthEncoder` 标准库闭包 | DONE（1 source） |
| 2 | `aiapi_domain` | 纯模型、策略和 port；JSON 完全移除在 P3-W4 验收 | DONE（INTERFACE + clock port；codec 债务交 P3-W4） |
| 3 | `aiapi_application` | Generation/Session/Account/Workspace use case 的可编译闭包 | DONE（1 source：Retool provision policy） |
| 4 | `aiapi_infrastructure` | HTTP/clock、DB store、配置、metrics adapter | DONE（21 sources） |
| 5 | `aiapi_transport` | 四类响应 sink、无业务依赖的 LogController | DONE（5 sources） |
| 6 | `aiapi_runtime` | lifecycle shutdown orchestration | DONE（1 source） |
| 7 | legacy ceiling | 只保留被后续明确前置阻断的 source | DONE（67 → 39；最终删除 P8） |

该表不预先伪造 source 数字。精确文件归属必须从当前 compile/include graph
生成，并在本文“过程产物”逐闭包回填。

## 4. 过程产物（随实施回填）

| 产物 | 状态 |
|---|---|
| 当前 67 个 legacy source 的 include/link/third-party inventory | DONE（4.1～4.4；完整逐文件职责仍引用 source audit） |
| 每个 source 的候选 target 和阻断边 | DONE（29 已归属，39 个按后续前置分类） |
| 每个闭包的 target diff 与 source-owner 证据 | DONE（见 4.2/4.5/4.6） |
| 正式 target link DAG 和对应 layer-rules | DONE（CMake DAG 门禁已落地） |
| Provider/Drogon 静态注册的最终 ownership | DEFERRED P5/P6/P8（当前 ceiling 明确） |
| test 最小 target 链接与 stub 边界 | DONE（当前 `runtime + legacy`，DB stub 行为保真） |
| legacy 删除证据 | DEFERRED P8（`--require-no-legacy` 已可执行） |
| normal/coverage/ASan 与全量架构门禁 | DONE（262/262） |

### 4.1 开工 inventory 与判定口径

基线 commit `cbf1fba` 中除 `main.cc` 外共有 67 个 production
implementation，全部由 `aiapi_legacy` 持有。逐文件 include inventory 表明：

- 只有 `tools/ZeroWidthEncoder.cpp` 同时满足“只依赖标准库、无业务方向、被上层消费”；
- real clock/HTTP transport/DB store/config/metrics 是 infrastructure adapter，不能因代码
  只用标准库就误放进 platform；
- sink/Controller 都使用 Drogon/JsonCpp，但只有 sink 和 `LogController` 不直接知道 DB、
  Provider 或业务 singleton，可先形成 transport 闭包；
- domain/application 的大多数候选头仍暴露 `Json::Value`，或实现直接读取
  `drogon::app()`；不能通过给 application 偷链 Drogon/JsonCpp 来宣称闭包成立；
- Controller → DbManager/具体 Manager、Provider → Account/Session、Application →
  DbManager/ApiManager 是当前三类主要反向边。它们必须提取 port，不能靠 OBJECT library、
  link group 或未声明的静态符号边掩盖。

完整 production 文件的职责、方法和流程调用链继续以
[`source-audit-2026-08.md`](../audits/source-audit-2026-08.md) 为准；本文只记录本工作项新增的
target owner 和阻断边，避免复制另一份会漂移的源码审计。

### 4.2 已迁出的 29 个 implementation

| Target | 数量 | Implementation |
|---|---:|---|
| platform | 1 | `tools/ZeroWidthEncoder.cpp` |
| application | 1 | `accountManager/RetoolProvisionHealth.cpp` |
| infrastructure · HTTP/clock | 7 | `accountManager/{AccountHttpTransport,AccountClock,RetoolProvisionClock}.cpp`；`apipoint/chaynsapi/{ChaynsHttpTransport,ChaynsClock}.cpp`；`apipoint/retoolapi/{RetoolHttpTransport,RetoolClock}.cpp` |
| infrastructure · Chayns codec/policy | 2 | `apipoint/chaynsapi/{ChaynsModelCatalog,ChaynsMessageCorrelation}.cpp` |
| infrastructure · stores | 9 | `dbManager/account/{accountDbManager,accountBackupDbManager}.cpp`；`dbManager/channel/channelDbManager.cpp`；`dbManager/config/ConfigDbManager.cpp`；`dbManager/chaynsThread/chaynsThreadDbManager.cpp`；`dbManager/session/SessionDbManager.cpp`；`dbManager/metrics/{ErrorStatsDbManager,StatusDbManager}.cpp`；`dbManager/retoolWorkspace/RetoolWorkspaceDbManager.cpp` |
| infrastructure · config/metrics | 3 | `utils/ConfigValidator.cpp`；`metrics/{ErrorStatsConfig,ErrorStatsService}.cpp` |
| transport | 5 | `controllers/sinks/{ChatSseSink,ChatJsonSink,ResponsesSseSink,ResponsesJsonSink}.cpp`；`controllers/LogController.cc` |
| runtime | 1 | `utils/ApplicationShutdown.cpp` |

`aiapi_domain` 当前仍是 `INTERFACE` library，因为 domain 模型只有头文件；这不是 codec 已完成。
`aiapi_application` 已由 INTERFACE 升为 STATIC，并持有不依赖 Drogon/Trantor/DB concrete 的
Retool provision health policy。domain JSON 债务由 P3-W4 消除。

### 4.3 当前真实 target/link 图

```text
aiapi_platform (1)
  ↑
aiapi_domain (INTERFACE)
  ↑
aiapi_application (1)

aiapi_infrastructure (21) ──> domain, platform, Drogon/OpenSSL/PostgreSQL
aiapi_transport (5)        ──> application, domain, platform, Drogon
aiapi_runtime (1)          ──> application, infrastructure, transport

aiapi_legacy (39, shrinking scaffold)
  └─> application, infrastructure, transport + current third-party libraries

aiapi executable
  └─> whole-archive(aiapi_legacy + aiapi_transport), aiapi_runtime
aiapi_test / shutdown fixture
  └─> aiapi_legacy, aiapi_runtime (ordinary archive extraction)
```

正式六 target 之间的 link 边严格等于 ADR-02 DAG。新增
`tools/arch/check_target_layers.py` 同时校验 target 类型、正式内部 link 边、source-list
登记和 legacy 上限；当前上限是 39，下一次提交只能下降。P8 最终退出必须运行
`--require-no-legacy`，因此脚手架不能被遗忘。

### 4.4 剩余 39 个 source 与阻断边

| 候选归属 | 数量 | 当前 source | 不能直接迁出的原因 |
|---|---:|---|---|
| application/account/workspace | 9 | `accountManager/accountManager.cpp`；`apiManager/{ApiFactory,ApiManager}.cpp`；`channelManager/channelManager.cpp`；`managedAccount/backends/{ClassicProviderAccountBackend,RetoolWorkspaceBackend}.cpp`；`managedAccount/service/ManagedAccountService.cpp`；`retoolWorkspace/{RetoolWorkspaceManager,RetoolWorkspaceService}.cpp` | concrete DB/HTTP/Manager ownership、service locator、Drogon 日志；ManagedAccount service 直接构造具体 backend |
| infrastructure/provider | 3 | `apipoint/chaynsapi/{chaynsapi,chaynsThreadReaper}.cpp`；`apipoint/retoolapi/retoolapi.cpp` | Provider adapter 反向访问 Account/Api/Channel/Session/Workspace 具体对象；需窄 port，不能让 infrastructure 链 application |
| transport/controller | 7 | `controllers/{AiApiController,AccountController,ChannelController,MetricsController,HealthController,RetoolWorkspaceController,RetiredProviderTombstone}.cc` | Controller 直接访问 DbManager、Provider/Manager concrete 或 concrete telemetry；目标 transport 只能依赖 application/domain/platform |
| application/session pipeline | 20 | `sessionManager/core/{Session,SessionCodec,ClientOutputSanitizer,GenerationService,GenerationServiceEmitAndToolBridge,RetiredProviderTelemetry,RequestAdapters}.cpp`；`sessionManager/tooling/{BridgeHelpers,BridgeProtocolCodec,StrictClientRules,ToolCallBridge,ToolCallValidator,XmlTagToolCallCodec}.cpp`；`sessionManager/continuity/{ResponseIndex,HistoryReplayBudget,OutboundBudget,TextExtractor,ContinuityResolver}.cpp`；`sessionManager/actionProtocol/{ActionProtocolCompiler,ActionProtocolAdapter}.cpp` | contracts/domain 仍含 JsonCpp；流程直接访问 ApiManager/Account/Channel/DB/Metrics/chatSession；部分策略直接读取 `drogon::app()` 或记录 Drogon 日志 |

这 39 个文件不会整批改名为 `AIAPI_RUNTIME_SOURCES` 来伪造 legacy 删除。逐文件闭包审计已经
证明它们分别被 P3-W4 JsonCpp、P5 service locator 或 P6 Provider/session contract 阻断。
ADR-11 v2 因而采用 ceiling strangler：阻断边消除后立即迁出，P8 才执行最终 no-legacy 门禁。
这是调整依赖顺序，不是取消删除目标。

### 4.5 当前批次证据

- source owner：69/69 production implementation owner/compile count 均为 1；
- legacy owner：67 → 39；正式 owner：platform 1、application 1、infrastructure 21、transport 5、runtime 1；
- include：103 headers、428 canonical、relative/basename/`..` 均为 0、CMake `src/` 根仍为 1；
- link：production 对仍含 `IMPLEMENT_RUNTIME` 的 legacy 和已接管 Drogon Controller 静态注册的
  transport 使用 whole-archive；`nm -C aiapi` 必须存在 `LogController` allocator/handler。测试对所有
  archive 保持普通提取，因此 `stub_db_collaborators.cpp` 仍可隔离真实数据库；
- coverage collector 已从只识别 `aiapi_legacy.dir` 改为识别六个正式 production library 和
  legacy/test gcda；production executable 的未执行 gcda 仍不进入口径；
- normal、coverage、ASan：262/262 PASS；architecture/cycle/layer/startup/provider-retirement
  门禁通过。

### 4.6 application 首个实现：Retool provision health

原 `RetoolProvisionHealth.cpp` 只因直接使用 `trantor::Date` 无法进入 application。当前拆分为：

```text
AccountManager (legacy caller)
  ├─ IKeyValueConfigStore
  ├─ IRetoolProvisionClock
  └─ RetoolProvisionHealth policy (aiapi_application)

main/runtime
  └─ makeSystemRetoolProvisionClock
       └─ local timestamp codec/system_clock (aiapi_infrastructure)
```

- domain port 只暴露 `system_clock::time_point`、format/parse，不包含 Trantor/Drogon；
- application 决定三次失败阈值、30 分钟窗口和 `until > now` 的严格边界；
- infrastructure 负责本地 DB timestamp codec，并拒绝非法日期、尾随字符和 mktime 归一化；
- main 在 `AccountManager::init()` 前显式注入，`check_startup_wiring.py` 防止漏接线；
- fake clock 测试精确验证 `fake:1000000`、`fake:1001800` 和边界，无真实时间等待。

## 5. 退出门禁

- `aiapi_platform/domain/application/infrastructure/transport/runtime` 六个正式库存在；
- `AIAPI_LEGACY_SOURCES` ceiling 不高于 39；最终删除门禁登记到 P8；
- 每个生产 `.cpp/.cc` owner/compile count 仍恰好为 1；
- test target 不编译生产源，只链接所需正式库；
- target link 方向与 layer rules 一致，无反向边、无新增豁免、无环；
- 428 个当前自有 include 的完整路径规则不回退，CMake 子目录 include root 为 0；
- normal/coverage/ASan 全量测试及 architecture/startup/provider-retirement 门禁通过。

## 6. 回滚

每个 carve-out 是独立可回滚闭包。失败时将该闭包的 source owner 放回
`aiapi_legacy`，恢复该 target 的 link diff，然后从空 build directory 验证。不恢复
`PROJECT_SOURCES`，不加回子目录 include root，不为了暂时编译通过而违反 target DAG。
