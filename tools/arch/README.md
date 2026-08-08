# tools/arch —— 模块依赖环门禁（C4 / R4）

## 为什么需要它

ADR-02 靠 `target_link_libraries` 强制分层，而 **CMake 不允许 static library 循环依赖**。
只要 `src/` 存在跨模块环，阶段 1 的 target 拆分就无法完成。

更重要的是：阶段 0.7 的三环表是**逐条人工定位**的 —— 边看得很准，
但**看不出九个模块合起来是一个环**。这就是本脚本存在的理由。

## 用法

```bash
# 报告模式（不判定）
python3 tools/arch/check_cycles.py

# 带 file:line 证据
python3 tools/arch/check_cycles.py --evidence

# 门禁模式：实测结果必须是基线的子集，否则退出码 1
python3 tools/arch/check_cycles.py --baseline tools/arch/cycles-baseline.json

# 修完环之后收紧基线（棘轮只能往紧了拧）
python3 tools/arch/check_cycles.py --write-baseline
```

## 退出码

| 码 | 含义 |
|---:|---|
| 0 | 通过（无新增环） |
| 1 | 存在超出基线的环或双向边 |
| 2 | **前提被破坏**：出现跨目录同名头文件，基名判据失效 |

## 判据说明（重要）

判据是「`#include` 的头文件**基名** → 该头文件在 `src/` 下的顶层目录」。

> **不要改回用 include 路径前缀判断模块归属。** v2.3 用过路径前缀，测出 **0 个环** —— 那是错的：
> 仓库里有 200 处 include 只写文件名，路径前缀判据完全看不见它们。

基名判据成立的前提是**头文件名全库唯一**。脚本每次运行都会先自检该前提，
不成立就直接退 2 —— 这同时也是 ADR-03 机械改写的守门条件
（重名状态下机械改写会静默改错目标）。

## 两个基线文件

| 文件 | 含义 |
|---|---|
| `cycles-baseline.json` | **当前实测态**（9 节点 SCC / 6 条双向边）。CI 用它防回归 |
| `cycles-target.json` | **阶段 0.7 验收标准**：SCC 仅剩 `{apipoint, sessionManager}`。阶段 0.7 完成时应能通过 |

阶段 0.7 的每一项（C1~C7）做完后，都应重跑 `--write-baseline` 收紧 `cycles-baseline.json`，
最终它应与 `cycles-target.json` 一致。

## 残留的那一个环

`{apipoint, sessionManager}` 由 `sessionManager/core/Session.cpp:6 -> chaynsapi.h` 造成，
是**真 DIP 违规**（domain 直接依赖具体 Provider），必须靠 `IChatProvider` port + 组合根注入才能断，
**已明确转阶段 2 与「消灭单例」同批处理**，不在阶段 0.7 范围内。
