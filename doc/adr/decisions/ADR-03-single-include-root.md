# ADR-03 include 路径收敛为单一根

| 项 | 值 |
|---|---|
| 状态 | 已接受，待实施 |
| 来源 | RFC-001 v2.5 §2（原行 218~229），P4 拆分外移 |
| 迁移落点 | 见 [`migration-plan.md`](../migration-plan.md) |
| 数字真值源 | [`architecture-baseline.md`](../architecture-baseline.md) |

---

## 决策与理由

`target_include_directories` 只保留 `src/`，所有 include 写全相对路径：

```cpp
#include "domain/session/Session.h"   // 而非 #include "Session.h"
```

**理由**：让依赖在源码中肉眼可见，Code Review 可直接发现跨层引用。

---


---

## 工作量量化（2026-08-07 实测）

报告：[`../reports/adr-02-03-include-audit.md`](../reports/adr-02-03-include-audit.md)

### 真实规模：211 处改写，此前从未量化

| 写法 | 数量 | 处置 |
|---|---:|---|
| 尖括号根相对 | 264 | 已合规，保留 |
| 双引号同目录 | **200** | **需改写** |
| 双引号相对路径（含 `../`）| **11** | **需改写** |

**那 200 处同目录 include 之所以能编译，完全依赖 `src/CMakeLists.txt:108-137` 暴露的 28 个目录。**
本 ADR 一旦把 include 根收敛为单一 `src/`，**这 200 处立即全部编译失败**。

原文只给了一个示例，读起来像风格调整，实为一次全库机械改写。

### 前置检查：已执行，前提成立

改写规则是「文件名 → 其在 `src/` 下的唯一相对路径」，前提是头文件名全库唯一。

> **本次已测：全库 `.h` 重名数 = 0。** 机械改写可直接脚本化，无需先改名。
>
> 但该检查**必须保留为阶段 1 守门项**：阶段 0.5 会删 openai/nexos、阶段 1~3 会移动大量文件，
> **改写脚本执行当天必须重跑一次**。今天为 0 不代表那天为 0。

### 执行方式与工期

**脚本机械改写 + 编译验证，非人工逐个修改。工期 +0.5 天。**

### 11 处相对路径 include 暴露的跳层信号

非测试文件仅 4 处，其中两处构成双向依赖：
`metrics/ErrorStatsService.h:6` → `dbManager/metrics/ErrorStatsDbManager.h`，
而 `ErrorStatsDbManager.h:11` → `metrics/ErrorEvent.h`。
**已提交阶段 0.7 核对**：确认是否属已登记的三个环，若否则为**第四个环**。
