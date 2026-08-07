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
