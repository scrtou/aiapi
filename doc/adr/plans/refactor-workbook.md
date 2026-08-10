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

当前是 **P4（阶段 4，AppContext、队列和停机）**，P4-W1 已 DONE，当前工作项 P4-W2 AppContext/Builder/runtime lifecycle。P3 全部退出门禁已通过：P3-W1～W3 建立 69/69 production owner/compile、单一 include 根和六个正式 target，29 个 implementation 已迁入，legacy ceiling 为 39；P3-W4 已完成 domain 模型与 JSON codec 分离，domain 层 JsonCpp 归零。剩余 target 迁移由后续 service-locator/Provider port 工作逐步解锁，P8 最终执行 no-legacy 门禁。
P4-W1 已于 2026-08-09 收口：C1～C7 全部 DONE。三 bool 状态位由单一四态 `State` 取代，`kDefaultCapacity = 1024` 背压上限与 Draining 拒绝递归入队生效，`waitUntilIdle(deadline)` 提供 drain 完成信号；transport 11 处按 QueueFull/终态区分 503 与 `Retry-After`，infrastructure 10 处失败打 ERROR 不静默，`main.cc` 显式 `start()` 且隐式 spawn 已移除；`enqueueLegacy` 兼容 shim 删除，生产调用点命中 0。退出门禁「23/23 调用点显式处理 `EnqueueResult`」已达成，并固化为 `tools/arch/check_enqueue_result.py`（CI 第 10 个门禁 step）。全量 282 用例 / 1513 断言在 normal、coverage、ASan 三构建下一致 PASS，ASan+UBSan 零报告，全部 10 个门禁 step rc=0，P1-W5 停机 harness 无回归。当前只允许执行 P4-W2，完成后进入下一项。


## 按 migration-plan 排列的工作项

| ID | migration 阶段 | 工作项 | 必交产物 | 状态 |
|---|---|---|---|---|
| P0-W1 | 阶段 0 | 当前真值与 clean baseline | [`P00-current-baseline.md`](../work-products/P00-current-baseline.md) | DONE |
| P1-W1 | 阶段 1 | gcov/llvm-cov 运行时覆盖基线 | [`P01-runtime-coverage.md`](../work-products/P01-runtime-coverage.md) | DONE |
| P1-W2 | 阶段 1 | Chayns 脱敏 fixture 与假上游 | [`P01-chayns-fixtures.md`](../work-products/P01-chayns-fixtures.md) | DONE |
| P1-W3 | 阶段 1 | Retool workflow/agent characterization | [`P01-retool-characterization.md`](../work-products/P01-retool-characterization.md) | DONE |
| P1-W4 | 阶段 1 | Generation/Account 权威实现 characterization | [`P01-generation-account-characterization.md`](../work-products/P01-generation-account-characterization.md) | DONE |
| P1-W5 | 阶段 1 | SIGTERM、队列、断连 harness | [`P01-shutdown-characterization.md`](../work-products/P01-shutdown-characterization.md) | DONE |
| P2-W1 | 阶段 2 | Provider 数据 dry-run 和归档/恢复脚本 | [`P02-provider-data-retirement.md`](../work-products/P02-provider-data-retirement.md) | DONE |
| P2-W2 | 阶段 2 | nexos/OpenAiProvider tombstone 与代码退役 | [`P02-provider-code-retirement.md`](../work-products/P02-provider-code-retirement.md) | DONE |
| P3-W1 | 阶段 3 | production target 唯一 source owner | [`P03-production-targets.md`](../work-products/P03-production-targets.md) | DONE |
| P3-W2 | 阶段 3 | 单一 include 根和完整路径 | [`P03-include-root.md`](../work-products/P03-include-root.md) | DONE |
| P3-W3 | 阶段 3 | 正式 target、首批闭包与 legacy ceiling | [`P03-layered-targets.md`](../work-products/P03-layered-targets.md) | DONE |
| P3-W4 | 阶段 3 | domain 模型与 JSON codec 分离 | [`P03-domain-codecs.md`](../work-products/P03-domain-codecs.md) | DONE |
| P4-W1 | 阶段 4 | 有界 executor 和四态队列 | [`P04-bounded-executor.md`](../work-products/P04-bounded-executor.md) | DONE |
| P4-W2 | 阶段 4 | AppContext/Builder/runtime lifecycle | 启动失败回滚、显式 ownership | DOING |
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
