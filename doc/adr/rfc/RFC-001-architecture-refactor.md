# RFC-001 aiapi 架构重构方案

| 项 | 内容 |
|---|---|
| 状态 | 已接受，已实施 |
| 当前版本 | v4.1 |
| 语言 | C++17 |
| Provider 目标范围 | 保留 chayns + retool；下线 nexos + 不可触达的 OpenAiProvider |
| 工期 | 不纳入本文；按门禁推进 |

## 1. 文档真值

| 文档 | 唯一职责 |
|---|---|
| 本 RFC | 目标、范围、目标架构和不可变原则 |
| [`decisions/`](../decisions/) | 当前有效架构决策 |
| [`migration-plan.md`](../plans/migration-plan.md) | 当前阶段、任务顺序、进入/退出门禁 |
| [`architecture-baseline.md`](../audits/architecture-baseline.md) | 当前机器审计快照及指标定义 |
| [`source-audit-2026-08.md`](../audits/source-audit-2026-08.md) | 当前 `src/` 文件、流程、线程和重写边界的事实审计 |
| [`target-architecture.md`](../design/target-architecture.md) | 目标文件树、target DAG 和对象所有权 |
| [`module-catalog.md`](../design/module-catalog.md) | 目标模块、类、方法和直接调用者 |
| [`flow-contracts.md`](../design/flow-contracts.md) | 启动/生成/账号/回收/停机调用契约 |
| [`refactor-workbook.md`](../plans/refactor-workbook.md) | migration 阶段内部的工作项、产物和状态；不定义独立顺序 |
| [`interface-drafts.md`](../design/interface-drafts.md) | 非契约性的接口示例 |
| [`CHANGELOG.md`](../history/CHANGELOG.md) | 历史纠错和版本演进 |

当前执行文档不再保留“先写错误结论、再在末尾撤回”的叙事。历史由 git 和 CHANGELOG 保存。

## 实施结论

P8-W1 已完成：全部 production implementation 已由六个正式 target 唯一拥有，`aiapi_legacy`
及其 source list 已删除；ProviderResult/重复 ErrorCode compatibility 已清理；ADR-09 HTTP/IO seam
由静态 selftest 和运行时配置接线共同保护。这个结论只证明 target/DAG 收口，**不证明物理目录已收口**。

P8-W2 已将仍留在历史顶层目录的实现迁入 canonical `application/`、`infrastructure/`、`transport/`
tree，并以 physical-layout gate 防止旧目录或错误层 source 复活。P8-W1 的历史证据、验证命令与回滚说明见
[`P08-transition-cleanup.md`](../work-products/P08-transition-cleanup.md)；P8-W2 的设计、验证和 clean
baseline 规则见 [`P08-physical-layout.md`](../work-products/P08-physical-layout.md)。实现与 baseline
刻意分为两个提交，以保证机器快照生成时工作树干净。


## 2. 问题

项目当前主要结构性风险为：

1. 单一 executable 和大量 include 目录使模块边界不可见；
2. 业务流程通过单例/Service Locator 获取依赖，生命周期和测试替身困难；
3. Provider 接口修改 `session_st&`，成功/失败依赖副作用；
4. chayns/retool 的网络、轮询、错误和取消语义未形成稳定边界；
5. GenerationService 与 AccountManager 中存在超长函数和重复/半迁移实现；
6. domain 当前混入 `Json::Value`，并非协议无关领域模型；
7. worker 队列缺完整背压与统一 deadline，停机契约曾存在互相冲突的时限；
8. 测试数量不少，但“被 include”不能证明代码在运行时被覆盖。

实时数量不在 RFC 内硬编码，见 `architecture-baseline.md`。

## 3. 目标

### 3.1 架构目标

- 依赖方向由 CMake target DAG 和架构规则共同强制；
- domain 只依赖标准库与 platform，不包含 JsonCpp/Drogon/DB/OpenSSL 类型；
- application 只通过 port 编排；
- Controller 不获取业务单例；
- Provider 使用不可变请求、结构化结果、deadline 和 cancellation；
- 所有持有线程/队列/连接的对象由 AppContext 显式拥有和关闭；
- 每次迁移一个垂直切片，旧路径随切片删除。

### 3.2 行为目标

- 保持现有 OpenAI 兼容 HTTP 协议行为；
- chayns 活跃路径在重构前后通过同一契约测试；
- retool 在重新启用或迁移前具备最小契约 fixture；
- event-loop 无阻塞 IO；
- 队列过载时明确拒绝/背压，不无限增长；
- SIGTERM 正常路径可取消、排空、join；超时路径可观测且不发生 UAF。

### 3.3 非目标

- 不更换 Drogon、数据库或 C++17；
- 不在重构提交中新增业务功能；
- 不为了满足行数阈值机械拆文件；
- 所有生产 Provider 必须继承薄 ProviderBase，但不强制共同的 HTTP/轮询模板；
- 不以测试用例数量、断言数量或 include 闭包替代真实覆盖和行为契约。

## 4. 目标结构

P8-W2 后，正式 target 和物理树使用同一组顶层层名；根 `main.cc` 只作为进程入口，`runtime/`
是唯一 composition root。

```text
src/
├── platform/                 Result、错误、deadline、日志与通用值设施
├── domain/{model,policy,port}/
├── application/
│   ├── account/              account selection/registration/token/worker
│   ├── channel/
│   ├── generation/{actionProtocol,continuity,contracts,core,tooling}/
│   ├── health/、metrics/、workspace/
├── infrastructure/
│   ├── account/、codec/、config/、executor/、metrics/、workspace/
│   ├── managedAccount/{backends,contracts,service}/
│   ├── persistence/{account,channel,chaynsThread,config,metrics,retoolWorkspace,session}/
│   └── provider/{chayns,limits,retool}/
├── transport/controllers/{codecs,sinks}/
├── runtime/
├── test/
├── CMakeLists.txt
└── main.cc
```

历史 `accountManager/`、`channelManager/`、`sessionManager/`、`dbManager/`、`managedAccount/`、
`metrics/`、`retoolWorkspace/`、`controllers/`、`apiManager/` 和 `models/` 均不是可用 `src/` 顶层目录。
详细树、迁移映射和机械约束见 [`target-architecture.md`](../design/target-architecture.md)。

具体依赖图见 ADR-01/02。

## 5. 核心决策摘要

1. **分层**：采用端口与适配器，不再把 infrastructure 描述成 domain 下方的线性层。
2. **边界强制**：CMake 负责 target DAG，架构脚本负责 include 方向；二者缺一不可。
3. **include**：单一 `src/` 根和完整路径只解决路径可见性，不冒充分层门禁。
4. **错误**：跨层预期失败使用 `Result<T, Error>`，按垂直切片迁移。
5. **依赖注入**：AppContext 替代业务 Service Locator；单个组件迁完即删除其旧访问器。
6. **Provider**：所有生产 Provider 继承薄 ProviderBase，公共入口使用 NVI；协议流程和重试/轮询等能力通过组合策略实现。
7. **并发**：统一绝对 deadline、取消、背压和停机状态机。
8. **Provider 下线**：使用可恢复的数据归档迁移；代码回滚与数据回滚均有明确步骤。
9. **IO/codec 边界**：HTTP、DB、JSON codec 不进入 domain/application；见 ADR-09/10。
10. **生产 target**：测试只链接生产库，禁止复制生产源；见 ADR-11。

完整决策见 [`decisions/README.md`](../decisions/README.md)。

## 6. Provider 下线边界

删除的是具体实现 `OpenAiProvider` 和 nexos Provider，不删除 OpenAI 兼容的公开 API、字段名或 retool 中合法的 `openaiResource*` 数据。

下线必须同时处理：

- 数据 dry-run、归档、恢复脚本；
- 历史路由的 410 tombstone；
- 配置校验；
- 指标、告警和 Dashboard；
- Provider 工厂注册、白名单、测试和 CMake；
- 观测一个成功发布边界后再删除 tombstone。

不得使用 `grep -ri openai src` 这种会误伤协议词汇的门禁；使用精确路径、类名、工厂 key 和路由清单。

## 7. 测试与审计策略

### 7.1 三类证据

| 证据 | 回答的问题 | 工具 |
|---|---|---|
| 行为契约 | 外部/业务行为是否保持 | fixture、characterization、集成测试 |
| 运行时覆盖 | 被改代码是否实际执行 | gcov/llvm-cov 行与分支覆盖 |
| 结构棘轮 | 结构是否继续恶化 | architecture_audit、cycle/layer rules |

`architecture_audit` 的 R2 只表示“高扇入头是否有直接/显式测试 owner”，不表示行覆盖。

### 7.2 结构规则

- R1：tooling 自由函数与 GenerationService 成员同名竞争，只减不增；
- R2：高扇入头无测试 owner，只减不增；
- R3：超长函数，只减不增；
- R4：依赖环与层方向；
- 扫描扩展名统一覆盖 `.h/.hpp/.cpp/.cc`。

### 7.3 完成判断

不再使用“单文件必须小于 800 行”“Provider 必须小于 400 行”作为主要验收。它们可作评审提示，真正门禁是：

- 职责和依赖边界；
- 复杂函数是否拆出可独立测试的规则；
- 行为契约与覆盖是否保护被改路径；
- 旧路径、旧注册、旧数据入口是否删除；
- 并发和停机契约是否通过集成测试。

## 8. 工程纪律

1. 行为修复与结构迁移分提交；
2. 每个提交可构建、可测试、可回滚；
3. 不复制实现制造长期双轨；
4. 每阶段先写进入条件和退出条件，再改代码；
5. 基线变化由工具生成，不手工维护数字副本；
6. 修改已有高风险文件前先确认对应行为测试实际执行该路径；
7. 工作区不干净时可以生成临时审计报告，但不得把它标成可复现发布基线。

## 9. 执行入口

后续施工只从 [`migration-plan.md`](../plans/migration-plan.md) 的“当前执行阶段”开始。该计划不包含工期，只表达严格依赖顺序、产出和门禁。
