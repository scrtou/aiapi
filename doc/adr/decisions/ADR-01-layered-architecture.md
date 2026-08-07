# ADR-01 采用四层架构 + 一个横切 platform target + 依赖倒置

| 项 | 值 |
|---|---|
| 状态 | 已接受，待实施 |
| 来源 | RFC-001 v2.5 §2（原行 190~208），P4 拆分外移 |
| 迁移落点 | 见 [`migration-plan.md`](../migration-plan.md) |
| 数字真值源 | [`architecture-baseline.md`](../architecture-baseline.md) |

---

## 决策与理由

```
Layer 4  transport      Drogon Controllers / Filters / Sinks
Layer 3  application    UseCase 编排，无 IO
Layer 2  domain         纯逻辑 + 接口定义（零外部依赖）
Layer 1  infrastructure Provider / DbManager / HttpClient / Clock
```

依赖方向严格自上而下；Layer 1 实现 Layer 2 定义的接口（ports）。

> **v2.3 术语澄清**：`platform/`（`Result.h` / `Logging.h` / `Config.h`）是**横切设施，不是第五层** —— 四层都可依赖它，它不依赖任何层。
> 它在 CMake 里确实是第 5 个 target，所以 §9 写「五层 target」指的是 **target 计数**，§3 目录结构同理。
> 统一口径：**架构分层 = 4，CMake target = 5（4 层 + platform）**。

**理由**：领域层无外部依赖是可测试性的前提，其余目标均依赖此条。

---
