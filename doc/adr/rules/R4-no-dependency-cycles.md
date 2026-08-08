# R4 · 模块间不得存在依赖环

## 规则

`src/` 下任意两个顶层模块目录之间，**不得形成新的依赖环**。
判定以 `tools/arch/check_cycles.py` 的实测结果为准，必须是 `tools/arch/cycles-baseline.json` 的子集。

## 为什么是构建约束而不是文档约束

ADR-02 已写明「文档约束必然腐化，构建约束不会」。依赖环尤其如此 ——
**前三项处方（C1/C2/C3）是一次性清理，若无门禁，环会以完全相同的方式长回来。**

实际后果不是「代码不够漂亮」，而是 **CMake 不允许 static library 循环依赖，阶段 1 会直接失败**。

## 执行方式

- CI：`.github/workflows/arch-cycles.yml`，对 `src/**` 的改动强制运行
- 本地：`python3 tools/arch/check_cycles.py --baseline tools/arch/cycles-baseline.json`

## 棘轮方向

基线**只能收紧，不能放松**。修完一个环就重跑 `--write-baseline` 并提交新基线；
如需放松基线，必须在 PR 描述中说明理由并链接对应 ADR。

## 附带的前提检查

脚本会自检「头文件名全库唯一」（当前重名数 = 0）。
该前提同时是 **ADR-03 机械改写的守门条件** —— 重名状态下 include 改写会静默改错目标。
阶段 0.5 删 Provider、阶段 1~3 移动文件后，**改写当天必须重跑**。


## 补充条款 · 分层边界（2026-08-08）

环检测有一个**结构性盲区**：中立层被污染时并不构成环，Tarjan 永远发现不了。
例如在 `domain/model/AccountData.h` 里 include 一个 `dbManager/` 的头 ——
只要 `dbManager` 不反向依赖 `domain`，全图仍是 0 环，门禁照样 PASS，
但 ADR-01 的方向约束已经失效了。

因此新增 `--layer-rules`：

| 项 | 内容 |
|---|---|
| 规则文件 | `tools/arch/layer-rules.json` |
| 当前条款 | `domain` 的 `allow_out` 为**空集** —— 中立层不得依赖任何业务模块 |
| 违规退出码 | **3**（区别于 1 = 新增环、2 = 前提被破坏）|
| CI | `arch-cycles.yml` 中作为独立强制步骤 |

**两项自测均已实跑**：当前代码 PASS(0)；
人为在 `AccountData.h` 注入一条指向 `dbManager` 的 include，判定 FAIL(3)，
并打印越界证据 `domain/model/AccountData.h:135`。

> 该注入同时制造了 `domain <--> dbManager` 双向边，所以环检测这次也会报 ——
> 但那是巧合。若污染指向一个不反向依赖 domain 的模块，**只有分层检查会报**。
