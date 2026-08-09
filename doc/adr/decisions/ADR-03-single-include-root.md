# ADR-03 include 使用单一仓库根与完整路径

| 项 | 值 |
|---|---|
| 状态 | 已接受，待实施 |
| 当前版本 | v3.0 |

## 决策

自有 target 只配置 `src/` 为 include 根，自有头文件统一写完整路径：

```cpp
#include <domain/model/SessionData.h>
#include <sessionManager/core/Session.h>
```

禁止依赖“当前目录恰好在 include path”而写 `#include "Session.h"`，也禁止 `../` 跨目录引用。

单一 include 根解决的是路径唯一性、可读性和可机械迁移性，不负责阻止跨层 include；跨层边界由 ADR-02 检查。

## 实施

1. 验证自有头文件 basename 全库唯一；
2. 按唯一映射脚本化改写；
3. 单独提交，不混入移动文件或行为修改；
4. 干净全量构建；
5. CI 禁止新增 basename include 和 `../` include。

头文件重名后仍可使用完整路径，但自动改写不再安全；唯一性只是迁移前置，不是永久命名限制。
