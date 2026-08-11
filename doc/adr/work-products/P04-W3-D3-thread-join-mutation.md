# P4-W3 / D3 变异矩阵：platform::joinUntil（缺口 H5）

实测环境：本机 build 目录，`ctest -R ^ThreadJoin_ --timeout 20`。
基线：原实现 7/7 通过；全量 322/322 通过。

## 变异定义

| 编号 | 变异内容 | 摘掉的语义 |
|------|----------|------------|
| M1 | `joinUntil` 忽略 `waitUntil` 返回值，径直 `join()` | 限时性（等价于退回裸 `join()`） |
| M2 | 超时路径改为 `thread.detach(); return false;` | 超时后线程所有权仍归调用方 |
| M3 | `ThreadCompletion::signal()` 改为空实现 | 线程自报完成 |

## 结果矩阵（· = 通过，X = 被杀）

| 用例 | M1 | M2 | M3 |
|------|----|----|----|
| T1 JoinsWithinBudgetWhenWorkerCompletes | · | · | X (abort) |
| T2 ReportsFalseWhenWorkerMissesDeadline | X (timeout 20s) | X (abort) | · |
| T3 TimeoutLeavesThreadJoinableForCaller | X (timeout 20s) | X (failed) | X (abort) |
| T4 NonJoinableThreadCountsAsJoined | · | · | · |
| T5 PastDeadlineDoesNotBlock | X (timeout 20s) | X (abort) | · |
| T6 SignalIsIdempotent | · | · | X (failed) |
| T7 SignalFromWorkerInterruptsLongWait | · | · | X (abort) |
| 合计被杀 | 3 | 3 | 4 |

三个变异各自都有用例捕获，无漏网变异。

## 必须记录的负面事实：T3 最初 是空转用例

T3 的第一版在 M2 下**通过**，也就是说它没有验证自己名字所声称的性质。
成因：超时后没有立刻断言 `worker.joinable()`。若实现 `detach()`，线程变为
non-joinable，随后的第二次 `joinUntil` 会走「非 joinable 视为已汇合」的快速路径
返回 true，末尾的 `CHECK(!worker.joinable())` 也自动成立——三条断言全部被
detach 语义免费满足。

修正：在超时返回后追加 `CHECK(worker.joinable())`。修正后 M2 下 T3 由
Passed 变为 Failed，原实现仍全绿。

教训与 D11 记录的同一条一致：绿色本身不是证据，只有「变异—失败」的对应关系才是。
本次若不做变异验证，会把一个不设防的用例当作 H5 的验收依据交付。

## 未覆盖 / 留给后续

- 超时后的**处置策略**（留痕、二次等待、还是升级为硬退出）不在本原语内，属 D5。
- 五处 join 的实际改造（AppContext / 各 worker owner）属 D4；本步只提供原语与验收。
