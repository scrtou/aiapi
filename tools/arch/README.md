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

## 门禁 4：dbManager 直接依赖棘轮（退出码 4）

```bash
python3 tools/arch/check_cycles.py --db-ratchet tools/arch/db-include-ratchet.json
```

业务层直接 `#include` dbManager 头文件的**文件级**白名单。新增即 FAIL。

**为什么需要第四道**：前三道都看不见这类退化。
`accountManager -> dbManager` 是单向边，不成环（门禁 1/2 无感）；
该模块也已在 `layer-rules.json` 的 `allow_out` 里（门禁 3 放行）。
于是「在已白名单模块里无限追加 include」成了倒置成果被悄悄侵蚀的通道。
粒度定在**文件**而非模块，就是为了堵这条路。

**两类判定**：
- `allowed_files`：冻结现状。出现清单外的文件即 FAIL。
- `must_stay_clean`：已完成依赖倒置的模块，必须保持零直连。
  这是显式表达意图，而不是依赖「它恰好不在清单里」。

**工作流**：
- 完成一个模块的倒置后，把它从 `allowed_files` 移除、加入 `must_stay_clean`。
- 解除直连后脚本会提示 `--write-db-ratchet` 收紧清单。减少不算违规，不会 FAIL。
- 确需放宽，显式改 JSON 并在提交信息里写明理由。

**判据坑（实测记录）**：
扫描复用与前三道相同的头文件基名索引，原因是路径判据会漏两类写法——
无路径的 `#include "ErrorStatsDbManager.h"`，以及 `.cc` 扩展名的文件。
手工 grep 曾因此漏报 4 个文件（10 vs 实际 14）。
