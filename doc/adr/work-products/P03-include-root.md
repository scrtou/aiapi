# P3-W2 · 单一 include 根与完整路径

| 项 | 值 |
|---|---|
| 状态 | DOING |
| 前置 | P3-W1 生产 source 唯一 owner 已完成 |
| 目标 | 自有头文件统一使用从 `src/` 起的完整 include，CMake 只暴露一个仓库 include 根 |
| 决策 | [ADR-03](../decisions/ADR-03-single-include-root.md) |

## 1. 本工作项边界

P3-W2 只收敛 include 语义和 usage requirements，不在本步 carve out 正式分层
target（P3-W3），也不在本步修改 domain JSON 模型（P3-W4）。

目标形式：

```cpp
#include <domain/model/AccountData.h>
#include <sessionManager/contracts/GenerationRequest.h>
#include <utils/ApplicationShutdown.h>
```

禁止形式：

```cpp
#include "AccountData.h"       // basename 依赖调用者 include path
#include "../utils/Foo.h"      // 跨目录相对路径
#include "../../domain/X.h"    // 跨层相对路径
```

第三方头文件和同目录的生成文件需要单独分类，不能被机械改写为
`src/` 自有路径。

## 2. 执行顺序

1. 生成当前 include inventory：自有 basename、`../`、解析失败、CMake include roots；
2. 重跑全库 header basename 唯一性，如有冲突先重命名，不做歧义自动改写；
3. 建立可 selftest 的 include 检查器，先用违规 fixture 证明会失败；
4. 按模块分批改写 include，每批立即 configure/build/test；
5. `aiapi_legacy` 的自有 include directory 收敛为 `${CMAKE_CURRENT_SOURCE_DIR}`；
6. 将检查器加入 CI，运行全量门禁并记录最终 inventory。

## 3. 过程产物（随实施回填）

| 产物 | 状态 |
|---|---|
| 变更前 include/CMake inventory | TODO |
| basename 唯一性结果 | TODO |
| 自有 include 分类与改写规则 | TODO |
| 检查器 + 负向 selftest | TODO |
| 分批改写记录 | TODO |
| 最终 CMake include-root 图 | TODO |
| normal/coverage/ASan 与架构门禁 | TODO |

## 4. 退出门禁

- 自有头 basename 冲突为 0；
- production/test 中可解析的自有 include 全部从 `src/` 起始，跨目录 `../` 为 0；
- production target 不再暴露 accountManager/apiManager/... 等多个子目录 include root；
- 负向探针证明 CI 门禁确实能拒绝 basename 和 `../` 回归；
- P3-W1 source ownership 仍为 68/68 且每个 compile 一次；
- clean normal/coverage/ASan build/test 和 architecture/cycle/layer/startup/provider-retirement 门禁通过。

## 5. 回滚

include 改写按批次提交。若某批编译失败，只回滚该批 include 和对应
CMake usage requirements；不恢复 `PROJECT_SOURCES`，不取消 P3-W1 的单 owner 门禁。
