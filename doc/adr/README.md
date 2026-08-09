# aiapi 架构重构文档入口

本目录只保存当前架构决策、目标设计、执行计划、审计基线和阶段产物。历史内容由 git 保存，
不再在目录中保留 `.bak` 或过期临时报告。

## 1. 目录结构

```text
doc/adr/
├── README.md                 本入口、阅读顺序和工作流程
├── rfc/                      重构目标、范围和不可变原则
├── decisions/                已接受的架构决策 ADR-01～ADR-11
├── design/                   目标结构、模块类目录、流程和接口草案
├── plans/                    唯一施工路线和阶段任务看板
├── audits/                   当前源码事实、机器基线和可读基线
├── rules/                    可自动执行的架构规则说明
├── work-products/            每个 P 阶段的设计、测试和回滚产物
└── history/                  文档版本演进记录
```

## 2. 每个目录和文件的作用

### `rfc/`

| 文件 | 作用 |
|---|---|
| [`RFC-001-architecture-refactor.md`](./rfc/RFC-001-architecture-refactor.md) | 定义为什么重构、目标是什么、范围和不可变原则；不记录当前任务状态 |

### `decisions/`

| 文件 | 作用 |
|---|---|
| [`README.md`](./decisions/README.md) | ADR 索引和状态 |
| `ADR-01`～`ADR-11` | 分层、CMake、include、C++17、Result、组合根、ProviderBase、并发、IO、codec、测试 target 等不可变决策 |

修改架构原则时先更新 ADR，再更新设计和计划。施工中不能绕过已接受 ADR。

### `design/`

| 文件 | 作用 |
|---|---|
| [`target-architecture.md`](./design/target-architecture.md) | 完整目标目录树、CMake target DAG、依赖方向和对象所有权 |
| [`module-catalog.md`](./design/module-catalog.md) | 目标模块、类、主要方法和直接调用者 |
| [`flow-contracts.md`](./design/flow-contracts.md) | 启动、生成、Provider、账号、回收、停机的类调用链和线程契约 |
| [`interface-drafts.md`](./design/interface-drafts.md) | C++17 接口草案；与 ADR 冲突时以 ADR 为准 |

### `plans/`

| 文件 | 作用 | 权威级别 |
|---|---|---|
| [`migration-plan.md`](./plans/migration-plan.md) | 当前阶段、阶段顺序、进入条件和退出门禁 | **唯一施工顺序** |
| [`refactor-workbook.md`](./plans/refactor-workbook.md) | migration 阶段内部工作项、状态和产物链接 | 任务看板，不定义第二套顺序 |

### `audits/`

| 文件 | 作用 |
|---|---|
| [`source-audit-2026-08.md`](./audits/source-audit-2026-08.md) | 当前 `src/` 文件、模块、流程、风险和重写边界事实 |
| [`audit-baseline.json`](./audits/audit-baseline.json) | 机器可读架构基线，禁止手改 |
| [`architecture-baseline.md`](./audits/architecture-baseline.md) | 从同一 JSON 生成的可读基线 |

### `rules/`

| 文件 | 作用 |
|---|---|
| [`R4-no-dependency-cycles.md`](./rules/R4-no-dependency-cycles.md) | 模块环和层方向门禁的定义、命令和判定规则 |

### `work-products/`

文件按 `P阶段-工作项` 命名，例如：

```text
P00-current-baseline.md
P03-production-targets.md
```

每份产物必须记录：输入、设计选择、类调用图、修改清单、测试结果、遗留风险和回滚方法。

### `history/`

| 文件 | 作用 |
|---|---|
| [`CHANGELOG.md`](./history/CHANGELOG.md) | 文档方案的版本演进和历史纠错；不作为当前施工入口 |

## 3. 开发工作流程

每次继续重构时严格执行：

1. 先查看 [`plans/migration-plan.md`](./plans/migration-plan.md) 的“当前执行阶段”；
2. 再到 [`plans/refactor-workbook.md`](./plans/refactor-workbook.md) 找相同 `P阶段` 的工作项；
3. 只执行当前阶段的工作项，不跨阶段提前重写；
4. 开始工作时将工作项标记为 `DOING`；
5. 将过程产物及时写入 `work-products/Pxx-*.md`；
6. 每个代码切片执行对应单测、全量测试和架构门禁；
7. 在工作产物中记录实际命令、结果、遗留问题和回滚方式；
8. 只有阶段退出门禁全部通过，才把工作项标为 `DONE`；
9. 最后修改 `migration-plan.md` 和 `refactor-workbook.md`的当前阶段，进入下一阶段。

## 4. 文档冲突时的优先级

```text
已接受 ADR
  > RFC 范围/原则
  > migration-plan 阶段顺序和门禁
  > target design/module/flow
  > refactor-workbook 状态
  > interface drafts
  > history
```

发现冲突时不得自行选择方便的版本，应先修正文档再继续开发。

## 5. 当前入口

当前阶段和下一动作始终以
[`plans/migration-plan.md`](./plans/migration-plan.md) 顶部“当前执行阶段”为准。
