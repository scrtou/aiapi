# ADR-02 用 CMake target 强制分层，而非依靠约定

| 项 | 值 |
|---|---|
| 状态 | 已接受，待实施 |
| 来源 | RFC-001 v2.5 §2（原行 209~217），P4 拆分外移 |
| 迁移落点 | 见 [`migration-plan.md`](../migration-plan.md) |
| 数字真值源 | [`architecture-baseline.md`](../architecture-baseline.md) |

---

## 决策与理由

拆分为独立 library target，由 **target 级 include 可见性 + 架构测试**共同阻断非法依赖。

> **P10 修正**：原文写「通过 `target_link_libraries` 的可见性在**编译期**阻断」，**表述不准确**。
> `target_link_libraries` 约束的是链接与 usage requirement 的传递；而「领域层 include 了 Drogon 头」
> 会不会报错，取决于该头是否在 include path 上可见 —— 若经全局路径、传递依赖或系统路径可见，
> 仅 include 一个纯声明头**不必然产生链接错误**。二者被混为一谈。

**理由**：P1 的根因是「约定没有强制力」。文档约束必然腐化，构建约束不会。

---

## 强制手段（P10 明确，替代「链接失败」单一手段）

| # | 手段 | 作用 |
|---:|---|---|
| 1 | 所有 include 目录仅经 `target_include_directories` 的 `PRIVATE/PUBLIC/INTERFACE` 暴露 | 让越界 include 在编译期真的找不到头 |
| 2 | **禁用全局 `include_directories()`** | 全局路径会让手段 1 失效 |
| 3 | 架构测试：扫描 `domain` 源文件的 `#include`，命中禁列即失败 | 兜住手段 1/2 的漏网 |
| 4 | CI 使用**干净构建目录**验证 | 防止残留缓存掩盖违规 |
| 5 | 可选：clang-tidy / IWYU 作为附加门禁 | 增量收紧 |

> **现状实测（P10）**：仓库自有 `CMakeLists.txt` 仅 3 个（根 / `src` / `src/test`），
> 根文件中 `include_directories` **零命中**。当前 include 路径的实际暴露方式**尚未查清**，
> 因此原文「越界会立即链接失败」这一断言**连现状基础都没有核实过**。
> 阶段 1 建 target 前，需先做一次 include path 暴露方式的实测。


---

## 现状实测结果（2026-08-07）

本 ADR 原文要求「阶段 1 建 target 前，需先做一次 include path 暴露方式的实测」。已完成。
报告：[`../reports/adr-02-03-include-audit.md`](../reports/adr-02-03-include-audit.md)

### 核心发现：手段 2 无对象，手段 1 有条款漏洞

仓库自有 CMakeLists 共 3 个（根 16 行薄壳 / `src` / `src/test`），
**没有任何全局 `include_directories()`** —— 手段 2 假设的敌人不存在。

真正的问题是 `target_include_directories` **被用成了全局**：`src/CMakeLists.txt:108-137`
一次暴露 **28 个子目录**，任何 `.cpp` 都能用文件名直接命中其它模块的私有头。

**手段 1 只约束「经由什么机制暴露」，未约束「暴露多少」，按原文当前仓库可判定合规。**
这是条款漏洞。

### 手段表修订

| # | 修订后 |
|---|---|
| 1 | 仅经 `target_include_directories` 暴露，**且每个 target 至多 1 个目录**（`src/` 根）。当前 28 个 |
| 2 | 禁用全局 `include_directories()` —— **降级为防退化约束，当前无对象，不计工作量** |
| 3 | 架构测试扫描 `domain` 的 `#include`（不变）|
| **6** | **（新增）CI 检查各 target 的 include 目录列表必须一致** —— `src/` 28 个（:108）、`src/test/` 8 个（:96），两份内容不同，已漂移 |

### P12 补充：测试与主程序的耦合方式

`src/test/CMakeLists.txt:58-93` 用 `PROJECT_SOURCES` 逐个列举 **32 个**主程序 `.cpp`，
与测试源一起 `add_executable` —— **测试不链接主程序产物，而是重新编译其中一部分**。
主程序源文件约 80 个，**其余六成测试侧根本不编译**；新增源文件若不同步登记则**静默漏测**，
且与根 CMakeLists「源清单只维护一处」的注释直接矛盾。

阶段 1 建 library target 后测试应改为链接，`PROJECT_SOURCES` 消失（顺带收益，不计工期）。
