# RFC-001 迁移执行计划

> 本计划不包含工期。阶段按依赖关系推进；只有退出门禁全部满足后才进入下一阶段。
>
> 源码事实以 [`source-audit-2026-08.md`](../audits/source-audit-2026-08.md) 为准。该审计确认当前实现仍是
> 单 executable、宽 `APIinterface`、JsonCpp domain、多个业务 singleton 和无界队列；因此下面的
> “已落地”只表示安全修复或门禁，不表示目标架构已经实现。
>
> 目标文件树、类方法目录和流程调用链分别见 [`target-architecture.md`](../design/target-architecture.md)、
> [`module-catalog.md`](../design/module-catalog.md) 和 [`flow-contracts.md`](../design/flow-contracts.md)。每个工作包
> 的设计/测试/回滚产物登记在 [`refactor-workbook.md`](./refactor-workbook.md)。

## 0. 当前状态

状态符号：✅ 已验证完成｜🔄 当前工作｜⬜ 未开始｜⛔ 被前置条件阻塞

| 项 | 状态 | 证据/说明 |
|---|:---:|---|
| C++17 固定 | ✅ | ADR-04，主程序与测试一致 |
| 非流式生成移出 event-loop | ✅ | N1 已落地 |
| shutdown 后队列不复活 | ✅ | N2 回归测试已存在 |
| Reaper stop 接入 | ✅ | N3 已落地 |
| 模块依赖环 | ✅ | `check_cycles.py` 当前为 0 环 |
| layer rules 基础门禁 | ✅ | 当前检查通过 |
| 架构审计 v3 口径 | ✅ | 已改为直接/显式 test owner，并覆盖 `.cc` |
| 可复现架构基线 | ✅ | 基于 clean commit `544bf44` 生成，CI 拒绝 dirty baseline |
| 运行时行/分支覆盖基线 | ✅ | P1-W1 gcov 真实执行基线；不能由 R2 替代 |
| Provider 下线 | ⬜ | 必须先完成阶段 1 安全网和可恢复数据迁移 |
| 独立 CMake libraries | ⬜ | 当前仍为单 executable |
| domain 去 JsonCpp | ⬜ | 当前为已登记迁移债务 |
| AppContext/业务单例清理 | ⬜ | 已有部分 port 试点，不代表完成 |
| Provider 瘦端口 | ⬜ | 旧 `APIinterface/session_st&` 仍存在 |
| `src/` 全量职责审计 | ✅ | `source-audit-2026-08.md`；已逐模块/流程登记所有权和重写边界 |
| 流程线程/错误/取消契约 | 🔄 | 审计已完成，四条集成测试和假上游尚未建立 |

**当前执行阶段：阶段 1（行为安全网），当前工作项 P1-W2（Chayns 脱敏 fixture 与假上游）。**

---

## 阶段 0 · 当前真值与门禁收口

### 目标

让后续每一步都基于可复现、含义准确的指标，而不是陈旧数字或伪覆盖。

### 任务

- [x] 架构审计统一扫描 `.h/.hpp/.cpp/.cc`；
- [x] R2 改为“高扇入头无直接/显式 test owner”，不再把传递 include 称为覆盖；
- [x] selftest 同时验证正向 owner 和“传递 include 不产生 owner”的负向不变量；
- [x] RFC、ADR、接口草案和执行计划只保留当前有效结论；
- [x] 从同一审计 payload 生成 `audit-baseline.json` 和 `architecture-baseline.md`；
- [x] CI 运行 audit selftest、ratchet、cycle、layer、startup wiring 和 test registration；
- [x] 建立“发布基线必须来自干净 commit”的 CI 规则；baseline 模式拒绝 `git.dirty != false`。

### 退出门禁

```bash
python3 tools/architecture_audit.py --selftest
python3 tools/architecture_audit.py --baseline doc/adr/audits/audit-baseline.json
python3 tools/arch/check_cycles.py \
  --baseline tools/arch/cycles-baseline.json \
  --layer-rules tools/arch/layer-rules.json
ctest --test-dir build --output-on-failure
```

- baseline 文件明确记录 commit 和 dirty 状态；
- Markdown 数字从同一份 JSON 生成或逐项对账；
- 文档中没有另一个“当前总工期/当前数量”副本。

---

## 阶段 1 · 行为安全网

### 目标

保护即将删除和重写的真实生产路径。测试必须证明行为，不以 include 或断言数量冒充覆盖。

### 任务

#### 1.1 运行时覆盖

- 增加 `AIAPI_ENABLE_COVERAGE` 构建选项；
- 生成行/分支覆盖报告；
- 记录以下高风险路径的当前覆盖：
  - `chaynsapi::generate/postChatMessage`
  - `GenerationService::transformRequestForToolBridge`
  - `GenerationService::emitResultEvents`
  - AccountManager 账号选择、失效、回滚、池重建
  - shutdown/BackgroundTaskQueue
- 对被修改文件采用“覆盖不下降”棘轮，不设全库虚假百分比目标。

#### 1.2 chayns 契约

- 从 HAR 提取并脱敏 thread create/message/poll/delete/model/read fixture；
- 动态 ID、时间戳、token 归一化；
- JSON 使用语义比较，只有公开 wire 格式需要逐字节 golden；
- 假时钟测试轮询退避、deadline、空响应、乱序、重复消息；
- 假上游跑 chat 非流式、chat 流式、responses 非流式、responses 流式冒烟。

#### 1.3 Characterization

- 锁定 ToolBridge 请求转换、forced tool、参数归一和 emit 顺序；
- 测试必须调用生产实际实现，R1 同名竞争项逐个消除；
- AccountManager 使用 store/http fake 验证配额、轮换、失效、回滚和重载；
- 建立 SIGTERM 测试 harness，先记录当前行为。

#### 1.4 审计映射（不可跳过）

行为安全网必须覆盖审计中列出的**真实生产权威实现**，不能只测同名 helper 或 stub：

| 流程 | 生产入口 | 必须锁定的副作用 |
|---|---|---|
| Chat/Responses 生成 | `GenerationService::runGuarded` → `executeProvider` → `APIinterface::generate` | session 连续性、ResponseIndex、tool bridge、sink 事件顺序 |
| Chayns 上游 | `chaynsapi::postChatMessage` | 账号租约、thread create/message、轮询 deadline、换账号、台账 |
| Retool 上游 | `retoolapi::requestWorkflow/requestAgent` | workspace 亲和、usage 计数、workflow/agent wire 格式 |
| 账号生命周期 | `AccountManager::{add,delete,checkToken,autoRegister}` | store 事务、状态转换、失败回滚、后台任务 |
| 停机 | `main` + `BackgroundTaskQueue` + Account/Session/Reaper workers | 广播取消、队列 drain、join 顺序、超时兜底 |

阶段 1 的退出报告必须按上述入口列出“执行次数/分支覆盖/假上游响应”，否则不得进入代码重写。

### 退出门禁

- chayns fixture 全部离线、无真实凭据、无真实时间依赖；
- 被重构的四条主路径在 coverage 报告中确实执行；
- 行为测试对生产实现做一次受控突变时能够失败；
- 全量测试和 ASan 基础运行通过。

---

## 阶段 2 · 可恢复地退役 nexos/OpenAiProvider

### 不可变决策

- 保留 chayns/retool；
- 删除具体 `OpenAiProvider` 和 nexos；
- 保留 OpenAI 兼容公开协议与合法的 `openai*` 业务字段。

### 2.1 数据预检与 dry-run

在目标部署数据库执行只读报告，记录环境、schema 版本、行数、主键范围和关联表：

```sql
SELECT api_name, COUNT(*)
FROM accounts
WHERE api_name IN ('nexosapi', 'openai')
GROUP BY api_name;
```

必须查询外键/关联表，不能因为当前观察到少量记录就跳过迁移设计。

### 2.2 可恢复迁移

交付两个经当前 schema 验证的幂等脚本：

```text
tools/migrations/retire_providers_v1.sql
tools/migrations/restore_retired_providers_v1.sql
```

退役脚本在一个事务中：

1. 创建带 `retirement_id`、原主键和完整 row snapshot 的 archive 表；
2. `INSERT ... ON CONFLICT DO NOTHING` 归档目标记录；
3. 对账归档数与待删除数；
4. 对账成功后删除或禁用原记录；
5. 写入 migration marker；
6. 任一步不一致则 rollback。

恢复脚本从同一 retirement_id 恢复，遇主键冲突必须报告并终止，不静默覆盖。

### 2.3 兼容边界

- 已知 nexos 历史路由返回 410 和迁移说明，不做不明确跳转；
- 记录 tombstone 路由调用指标；
- 旧 provider 配置在启动时给出明确错误和键名；
- 清理 Dashboard/告警/指标中的 provider 维度；
- 一个成功发布边界且 tombstone 调用为 0 后，才允许删除 tombstone。

### 2.4 代码删除顺序

1. OpenAiProvider 单独删除；
2. 数据迁移 dry-run；
3. 执行归档迁移；
4. nexos 路由 tombstone；
5. 删除 nexos 实现、账号逻辑、配置、测试、工厂注册和 CMake；
6. 删除无调用 tombstone。

### 精确门禁

禁止 `grep -ri openai src`。使用：

- `src/apipoint/openai/` 不存在；
- `src/apipoint/nexosapi/` 不存在；
- `OpenAiProvider`、`nexosapi` 工厂 key 和 provider 白名单精确命中为 0；
- OpenAI 兼容路由契约仍通过；
- archive/restore 脚本在数据库副本上往返对账成功；
- 代码回滚和数据恢复分别演练。

---

## 阶段 3 · 构建边界、include 与 domain 净化

### 3.1 先消除测试源码复制

1. 把除 `main.cc` 外的现有生产源码放入临时 `aiapi_legacy` library；
2. 主程序和测试共同链接它；
3. 删除测试侧 `PROJECT_SOURCES`；
4. CI 检查每个生产源只属于一个生产 target。

`aiapi_legacy` 是迁移脚手架，最终必须删除。

### 3.2 include 收敛

- 重跑 basename 唯一性检查；
- 脚本改写 basename/`../` include 为从 `src/` 起的完整路径；
- 只保留单一 include 根；
- 添加 CI 防回归。

### 3.3 建正式 target

依次从 legacy 中 carve out：platform → domain → application → infrastructure → transport。每次移动一个可编译闭包并立即从 legacy 删除。

### 3.4 domain 去 JsonCpp

按模型逐个迁移：

1. 为现有 `fromJson/toJson` 写 codec 往返测试；
2. 建 edge codec；
3. domain model 改为纯字段；
4. 修改调用点；
5. 删除 domain JSON 方法和 include；
6. 收紧 layer debt 清单。

优先顺序：ProviderResult/SessionData → AccountData/ChannelInfo → RetoolWorkspaceInfo/ErrorEvent。

### 退出门禁

- 五个正式 library target 建立，legacy target 为空并删除；
- 测试只链接生产 library；
- domain 不含 `Json::` 和第三方 IO 头；
- include 根唯一；
- clean build、测试、R4 全部通过。

#### 3.5 按源码审计确定的正式 library 闭包

禁止一次性把整个 `src/` 搬进一个新的“大库”后宣称完成。每个 target 必须是可编译闭包，
并在移动后从旧 target 删除源文件：

1. `aiapi_platform`：Result/Error/Deadline/取消/时钟/日志 port；
2. `aiapi_domain`：纯模型、连续性/账号/工具策略、所有 port；
3. `aiapi_application`：Generation pipeline、Session/Account/Workspace use case；
4. `aiapi_infrastructure`：Drogon HTTP、ProviderBase/Provider 实现、DB stores、executor；
5. `aiapi_transport`：Controllers、filters、SSE/JSON sinks、HTTP codec；
6. `aiapi_runtime`：AppContext、wiring、生命周期；`aiapi` 仅含 `main.cc`。

`source-audit-2026-08.md` 第 5 节是 target 所有权清单。测试只能链接这些库，禁止再次维护
`PROJECT_SOURCES`。

---

## 阶段 4 · AppContext、队列和停机

### 4.1 AppContext 骨架

- AppContext 先拥有配置、store、executor 和生命周期对象；
- main 只负责配置 → build → run → shutdown；
- build 失败返回 Result，不留下半启动线程；
- shutdown 接受统一绝对 deadline 且幂等。

### 4.2 BackgroundTaskQueue

- 三 bool 改四态状态机；
- 定义容量/并发上限与 EnqueueResult；
- 所有调用点处理失败；
- Draining 拒绝递归新任务；
- 提供完成通知供 AppContext `wait_until`。

### 4.3 取消链路

- 客户端断连 → token；
- Gate/停机 → token；
- HTTP timeout ≤ 剩余 deadline；
- 轮询/退避改可取消 wait_until；
- 记录暂时不能取消的 DB/HTTP 边界。

### 4.4 生命周期迁移

按 executor → metrics worker → AccountManager workers → chat session cleanup → Reaper 的依赖关系建立；停机按 ADR-08 广播、drain、等待、join、关闭 IO。

### 退出门禁

- event-loop 阻塞路径为 0；
- 业务队列有背压；
- 正常 SIGTERM 路径全部 join；
- deadline 超时路径不继续析构活动线程所访问对象；
- 空闲/阻塞 HTTP/轮询/积压/断连五种停机集成测试通过；
- ASan/TSan 通过。

#### 4.5 阶段边界调整

必须先重写 `BackgroundTaskQueue` 为有界 executor（`Accepted/QueueFull/ShuttingDown/Stopped`），
再把 Provider 的 HTTP/轮询改为 `ProviderCallContext`。否则取消 token 无法从 Controller 传播到
队列，Provider 迁移会把当前“bool enqueue + 忽略返回值”的丢任务行为固化到新接口。

---

## 阶段 5 · 消除业务 Service Locator

### 顺序

1. DbManager/store wrapper；
2. Channel/Retool workspace/metrics 等无后台线程服务；
3. ResponseIndex/SessionExecutionGate 等有状态服务；
4. AccountManager/chatSession/Reaper/TaskQueue 等生命周期服务。

每个组件：最小 port → 现有实现适配 → AppContext 构造 → 调用闭包切换 → 删除访问器。

### 退出门禁

- domain/application 项目自有 `getInstance()/instance()` 为 0；
- 例外仅在显式 allowlist；
- 已迁移组件无新旧构造双轨；
- Controller 只依赖 use case；
- 全量测试及停机测试通过。

#### 5.1 实际迁移顺序（依据调用图）

1. `ProviderRegistry/ProviderRouter`（替换 `ApiFactory/ApiManager`）；
2. `SessionStore`、`ResponseIndex`、`ExecutionGate`；
3. `AccountCatalog/ChannelCatalog/RetoolWorkspaceCatalog` 及其 store adapter；
4. `MetricsSink/ReadinessProbe`；
5. `ThreadReaper`、`AccountWorkerSupervisor`、`SessionJanitor`；
6. 最后迁移 Controllers，并删除所有 `getInstance()/instance()` 访问器。

---

## 阶段 6 · Provider 与 Result 垂直切片

### 6.1 Result 基础

- 落地 Result/Error/Deadline/CancellationToken；
- 合并重复 ErrorCode；
- 开启 nodiscard 门禁；
- 保留 ErrorEvent 为独立观测模型。

### 6.2 chayns 切片

1. 建不可变 ProviderRequest 和结构化 ProviderResponse；
2. 现有 chayns 通过 adapter 实现 IChatProvider；
3. ProviderCallContext 传入 deadline/token/sink；
4. application 使用 Result；
5. transport 统一映射 Error；
6. 删除 chayns 对 `session.response` 的写入/回读；
7. 契约与覆盖报告验证行为不变。

### 6.3 retool 切片

重复同一 port contract；workflow/agent 差异留在实现内部，不为迎合 chayns 制造空钩子。

### 6.4 ProviderBase 与真实共性

两家都必须继承薄 ProviderBase。ProviderBase 只负责 cancellation/deadline 前置检查、异常转换、tracing/metrics、结果合法性和单次错误上报，不保存请求级状态，也不规定 HTTP/轮询/SSE 流程。

Retry、timeout、error mapping、metrics、polling 的协议细节继续使用独立 policy/decorator。不得因为继承要求而制造空钩子。

### 退出门禁

- 旧 `APIinterface::generate(session_st&)` 删除；
- Provider 的 session 副作用和项目单例访问为 0；
- 两家通过同一 contract suite；
- 所有阻塞边界受 deadline/cancellation 控制；
- Error → HTTP 只有一个 transport 出口。

#### 6.5 Provider 方法拆分清单

旧 `APIinterface` 的方法必须逐个归属，禁止在新端口上保留空实现：

| 旧方法 | 新所有者 |
|---|---|
| `generate(session_st&)` | `IChatProvider::generate(ProviderRequest, ProviderCallContext)` |
| `getModels/checkModels` | `IModelCatalog`（只读快照/刷新策略） |
| `checkAlivableTokens` | `IProviderHealth` 或 Account token workflow |
| `init` | `ProviderFactory`/`AppContext::build`，失败返回 Result |
| `afterResponseProcess` | application `ProviderResponseFinalizer`（仅保留确有业务语义的步骤） |
| `eraseChatinfoMap/transferThreadContext` | `IProviderThreadContext`，由 SessionCommitter 显式调用 |

迁移完成的 Provider 不得再 include `sessionManager/core/Session.h`，不得访问 ApiManager、AccountManager
或任何 DB singleton。

---

## 阶段 7 · 拆解 GenerationService 与 AccountManager

### 7.1 先处理半迁移重复实现

- 为 R1 四个同名竞争项确定生产权威实现；
- 先用 characterization 锁行为；
- 将权威实现移入专职组件；
- 调用点显式命名空间/对象调用；
- 删除成员版或残废版，不保留转发壳作为永久结构。

### 7.2 Generation pipeline

按行为边界提取纯规则：请求规范化、连续性、预算、工具定义编码、工具解析、校验、参数规范化、输出净化、emit。

只有具备独立状态/策略和测试价值的步骤才成为 Stage。先拆超长函数，再决定文件布局。

### 7.3 AccountManager

优先提取：

- 账号选择/轮换纯策略；
- 状态转换和回滚规则；
- repository 适配；
- token refresh；
- registration workflow；
- health tracking。

避免按“一个大文件拆成五个类”机械分配；每个组件必须有明确输入、输出和所有者。

### 退出门禁

- R1 为 0；
- R3 棘轮持续下降；
- 修改过的复杂函数有分支覆盖和行为测试；
- 不以文件行数作为唯一完成条件；
- 旧实现删除。

#### 7.4 重写边界

`GenerationServiceEmitAndToolBridge.cpp` 与 `accountManager.cpp` 已超过单一职责可维护范围；在
characterization 通过后允许整体替换，但必须保持同一 port contract。不得把旧大类复制成多个
“Facade + Legacy*”并长期共存；每切一条流程立即删除旧实现。

---

## 阶段 8 · 收口

- 删除 legacy target、旧 APIinterface、重复 ErrorCode、无效配置和过渡 allowlist；
- 删除所有已归零 layer debt；
- 重新生成干净发布基线；
- clean build、全量测试、coverage、ASan/TSan、架构门禁、SIGTERM 集成测试通过；
- 更新 README 架构与运维章节；
- 为 Provider 数据 archive 保留独立运维说明，不随代码清理丢失；
- RFC 状态改为“已实施”。

## 每阶段通用规则

1. 开工前确认进入条件；
2. 结构修改与行为修复分提交；
3. 新实现接通时同步删除旧实现；
4. 数据、路由、配置、指标视为同一迁移单元；
5. 失败回滚必须说明是代码回滚、配置回滚还是数据恢复；
6. 基线只能由工具生成；
7. 任一阶段发现前置假设不成立，先更新 ADR/计划再继续编码。
8. 每个工作包开始前将 `refactor-workbook.md` 标记为 `DOING`，完成后附上调用图、测试结果和回滚说明，再标记 `DONE`。
