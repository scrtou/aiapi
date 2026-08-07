# ADR-02 用 CMake target 强制分层，而非依靠约定

| 项 | 值 |
|---|---|
| 状态 | 已接受，待实施 |
| 来源 | RFC-001 v2.5 §2（原行 209~217），P4 拆分外移 |
| 迁移落点 | 见 [`migration-plan.md`](../migration-plan.md) |
| 数字真值源 | [`architecture-baseline.md`](../architecture-baseline.md) |

---

## 决策与理由

拆分为独立 library target，通过 `target_link_libraries` 的可见性（PUBLIC / PRIVATE）
在**编译期**阻断非法依赖。

**理由**：P1 的根因是「约定没有强制力」。文档约束必然腐化，构建约束不会。

---
