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
