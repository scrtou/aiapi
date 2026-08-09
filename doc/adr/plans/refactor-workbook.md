# 重构执行工作簿

> **权威关系：** [`migration-plan.md`](./migration-plan.md) 决定阶段顺序、进入条件和退出门禁；
> 本工作簿只记录该阶段内部的工作项、负责人产物和当前状态。两者冲突时以 migration-plan 为准，
> 并立即修正本工作簿。

## 如何使用

1. 先在 `migration-plan.md` 查看“当前执行阶段”；
2. 只执行本表中相同 `P阶段` 的工作项；
3. 工作项开始时改为 `DOING`，产物写入 `work-products/Pxx-*.md`；
4. 阶段的全部退出门禁通过后，才能进入下一阶段；
5. 不按工作项编号跨阶段施工。编号只用于跟踪，不表示另一套路线。

状态：`TODO`、`DOING`、`DONE`、`BLOCKED`。

## 当前下一步

当前是 **P1（阶段 1，行为安全网）**，正在执行 P1-W3 Retool workflow/agent characterization。P0 已在 clean commit
`544bf44` 上生成发布基线，P1-W1/W2 已完成 coverage 基线和 Chayns 离线假上游。P1 全部退出门禁通过前，不能开始
P2 Provider 退役或 P3 production target 重构。

## 按 migration-plan 排列的工作项

| ID | migration 阶段 | 工作项 | 必交产物 | 状态 |
|---|---|---|---|---|
| P0-W1 | 阶段 0 | 当前真值与 clean baseline | [`P00-current-baseline.md`](../work-products/P00-current-baseline.md) | DONE |
| P1-W1 | 阶段 1 | gcov/llvm-cov 运行时覆盖基线 | [`P01-runtime-coverage.md`](../work-products/P01-runtime-coverage.md) | DONE |
| P1-W2 | 阶段 1 | Chayns 脱敏 fixture 与假上游 | [`P01-chayns-fixtures.md`](../work-products/P01-chayns-fixtures.md) | DONE |
| P1-W3 | 阶段 1 | Retool workflow/agent characterization | [`P01-retool-characterization.md`](../work-products/P01-retool-characterization.md) | DOING |
| P1-W4 | 阶段 1 | Generation/Account 权威实现 characterization | 生产调用入口、变异验证、分支覆盖 | TODO |
| P1-W5 | 阶段 1 | SIGTERM、队列、断连 harness | 空闲/阻塞/积压/断连当前行为报告 | TODO |
| P2-W1 | 阶段 2 | Provider 数据 dry-run 和归档/恢复脚本 | SQL、对账和往返演练报告 | TODO |
| P2-W2 | 阶段 2 | nexos/OpenAiProvider tombstone 与代码退役 | 410、配置/指标清理、精确门禁 | TODO |
| P3-W1 | 阶段 3 | production target 唯一 source owner | [`P03-production-targets.md`](../work-products/P03-production-targets.md) | TODO |
| P3-W2 | 阶段 3 | 单一 include 根和完整路径 | include 改写/防回归报告 | TODO |
| P3-W3 | 阶段 3 | domain 模型与 JSON codec 分离 | 往返 fixture、domain 第三方依赖为 0 | TODO |
| P4-W1 | 阶段 4 | 有界 executor 和四态队列 | 背压、drain、不可复活测试 | TODO |
| P4-W2 | 阶段 4 | AppContext/Builder/runtime lifecycle | 启动失败回滚、显式 ownership | TODO |
| P4-W3 | 阶段 4 | deadline/cancellation/shutdown | 五类 SIGTERM 集成测试、ASan/TSan | TODO |
| P5-W1 | 阶段 5 | ProviderRegistry/Router 注入 | 删除 ApiFactory/ApiManager service locator | TODO |
| P5-W2 | 阶段 5 | SessionStore/ResponseIndex/Gate 注入 | 删除 application 单例访问 | TODO |
| P5-W3 | 阶段 5 | Account/Channel/Workspace/Metrics 注入 | Controller 只依赖 use case | TODO |
| P6-W1 | 阶段 6 | Result/Error/ProviderBase 基础 | Result contract、NVI、继承静态门禁 | TODO |
| P6-W2 | 阶段 6 | Chayns Provider 垂直切片 | 无 session 副作用/单例，contract 通过 | TODO |
| P6-W3 | 阶段 6 | Retool Provider 垂直切片 | workflow/agent contract 通过 | TODO |
| P7-W1 | 阶段 7 | Generation pipeline 重写 | stage contract、旧实现删除、R1 归零 | TODO |
| P7-W2 | 阶段 7 | Account workflows 重写 | selector/state machine/workers/回滚测试 | TODO |
| P8-W1 | 阶段 8 | 过渡代码和 debt 清理 | clean baseline、完整发布验证 | TODO |

## 产物保存规则

- 工作项产物保存在 `doc/adr/work-products/Pxx-*.md`；
- 每份产物包含输入、设计、调用图、测试结果、遗留问题和回滚方式；
- 机器数字只从审计/覆盖工具生成；
- 代码和文档可以分提交，但门禁未通过不得将工作项标成 `DONE`。
