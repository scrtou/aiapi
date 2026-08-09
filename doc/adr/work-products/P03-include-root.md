# P3-W2 · 单一 include 根与完整路径

| 项 | 值 |
|---|---|
| 状态 | DONE |
| 前置 | P3-W1 生产 source 唯一 owner |
| 目标 | 自有头统一使用从 `src/` 起的完整 include，CMake 只暴露一个仓库 include 根 |
| 决策 | [ADR-03](../decisions/ADR-03-single-include-root.md) |

## 1. 输入、边界与不变量

P3-W2 只收敛 include 语义和 CMake usage requirements；未 carve out 正式分层
target（P3-W3），未修改 domain JSON 模型（P3-W4），也未改变任何业务行为。

自有头的唯一合法形式是：

```cpp
#include <domain/model/AccountData.h>
#include <sessionManager/contracts/GenerationRequest.h>
#include <utils/ApplicationShutdown.h>
```

禁止 basename、引号自有 include 和父目录相对路径：

```cpp
#include "AccountData.h"
#include "../utils/IoLoopResponseStream.h"
#include <../../apiManager/Apicomn.h>
```

第三方/标准库 include 保持原样。这个工作项只解析 `src/` 内自有头，
不把 Drogon、JsonCpp、OpenSSL 或 C++ 标准库头误改为仓库路径。

## 2. 变更前 inventory

### 2.1 头文件唯一性

`src/` 下自有 `.h/.hpp` 共 101 个，basename 冲突为 0。因此本次从
basename/相对 include 到完整路径的映射是唯一的，可以机械改写。

唯一 basename 是本次迁移前置，**不是永久禁止同名头**。迁移后完整
路径本身能解除歧义；检查器会报告同名 inventory，但不仅因同名就阻止构建。

### 2.2 include 分类

| 类别 | production | test | 合计 | 处置 |
|---|---:|---:|---:|---|
| 已是从 `src/` 起的自有完整路径 | 242 | 85 | 327 | 路径保留，其中引号形式改为尖括号 |
| 同目录/相对自有路径 | 82 | 7 | 89 | 改写为完整路径 |
| 只能依靠 CMake 子目录搜索的 basename | 6 | 0 | 6 | 改写为完整路径 |
| 第三方/标准库 | 567 | 152 | 719 | 不改 |

89 个相对自有 include 中，78 个是同目录或普通相对引用，11 个含
`..`。另有 142 个路径已完整但仍使用引号的自有 include。因此实际
文本改写为 237 处，分布于 127 个源/头文件。

### 2.3 CMake include roots

变更前共声明 37 个自有 include-root 条目：

- `aiapi_legacy`：`src/` 根 1 个 + accountManager/apiManager/... 子目录 27 个；
- `aiapi_test`：`src/` 根 1 个 + 7 个子目录；
- shutdown fixture：`src/` 根 1 个。

这使 `#include "ApiManager.h"` 是否编译取决于 target 偶然暴露了哪个子目录，
路径语义不是全库唯一。

## 3. 改写与构建图

### 3.1 改写规则

1. 优先按引用文件所在目录解析相对路径；
2. 其次按唯一 basename 映射自有头；
3. 目标必须真实存在于 `src/` 且后缀为 `.h/.hpp`；
4. 改写为 `<path/from/src/Header.h>`，保留 include 行其余内容；
5. 无法解析为自有头的 angle include 归为 external，不猜测、不改写。

该规则仅做语法等价改写，没有移动文件、更换类或改变调用顺序。

### 3.2 最终 include usage graph

```text
aiapi_legacy
  PUBLIC include: src/                 # 唯一自有 include 根
  │
  ├─> aiapi                           # 通过 target link 传播
  ├─> aiapi_test                      # 无自有 include-dir 副本
  └─> aiapi_shutdown_signal_fixture   # 无自有 include-dir 副本

self include
  └─> <module/submodule/Header.h>     # 对 src/ 根唯一解析
```

`aiapi_test` 和 fixture 不再单独声明仓库 include directory，而是从它们链接的
`aiapi_legacy` PUBLIC usage requirement 获取 `src/` 根。这为 P3-W3 按 target 拆分
usage requirements 建立了单一语义。

## 4. 防回归门禁

新增 `tools/arch/check_include_paths.py`，扫描 `.h/.hpp/.cpp/.cc`，并校验：

- 自有 include 必须精确命中 `src/<include>` 且使用尖括号；
- 同目录 basename、CMake 搜索 basename、歧义 basename 和任意 `..` 都失败；
- CMake 只能声明恰好一个解析到 `src/` 的自有根，不能暴露任何
  `src/` 子目录根；
- external angle include 不被当成自有头；
- 输出机器可重跑的 inventory，不用 grep 的部分匹配冒充解析。

CI `arch-cycles` 增加正向门禁，并有两个负向探针：

```text
#include "AccountData.h"  -> rc=1（basename self include）
#include "../outside.h"   -> rc=1（parent-relative include）
```

两个探针本地均已验证为 rc=1，且 trap/恢复清理探针文件。

## 5. 最终 inventory

```text
headers=101
duplicate_basenames=0
canonical=422
relative=0
basename=0
dotdot=0
ambiguous=0
external=719
cmake_src_roots=1
```

对比结果：

- 95 个路径不完整的自有 include 全部归零；
- 142 个引号完整自有 include 已统一为 angle 形式；
- 422/422 自有 include 现在都由单一 `src/` 根唯一解析；
- 719 个 external/标准库 include 未改；
- CMake 自有 include-root 条目从 37 收敛到 1，子目录 root 从 34 收敛到 0。

## 6. 验证记录

| 门禁 | 结果 |
|---|---|
| include-path gate | PASS；422 canonical，0 relative/basename/dotdot/ambiguous，1 CMake root |
| include negative probes | PASS；basename rc=1，`../` rc=1 |
| compile database 自有 `-I` inventory | PASS；唯一路径为仓库 `src/` |
| normal configure/build + `ctest` | 260/260 PASS |
| coverage configure/build + `ctest` | 260/260 PASS |
| coverage machine report | PASS；高风险函数运行时证据保留 |
| ASan configure/build + `ctest` | 260/260 PASS（`detect_leaks=0:halt_on_error=1`） |
| P3-W1 source ownership | PASS；68/68 每个 owner/compile 恰好一次 |
| test registration strict | PASS；260 声明 = 260 注册 |
| architecture selftest/ratchet | PASS |
| cycle/layer/startup/retired-provider | PASS |
| `git diff --check` | PASS |

覆盖率行/分支数字仍只由
[`P01-runtime-coverage-report.md`](P01-runtime-coverage-report.md) 机器生成，本文不复制第二份百分比。

## 7. 遗留与后续

- `aiapi_legacy` 仍是临时大库，P3-W2 只消除了 include path 偶然性，没有完成分层；
- 单一根使所有自有头对 target 可见，因此不能单靠编译器阻止跨层 include；
  P3-W3 仍必须用 target DAG + `check_cycles.py --layer-rules` 双重强制边界；
- 正式 target carve-out 时，每个 target 必须继续只传播同一 `src/` 根，不得
  为了修编译错重新加回子目录 include path。

## 8. 回滚

本步不改 schema 和数据。如需整体回滚：

1. 恢复改写前的 include token；
2. 恢复 `src/CMakeLists.txt` 和 `src/test/CMakeLists.txt` 的 include directories；
3. 移除 include gate 的 CI step；
4. 从空 build directory 重新 configure/build/test。

若后续单个 target carve-out 失败，只回滚该 target；不应回滚 P3-W2 的完整路径，
更不应加回子目录 include root 来隐藏错误的模块边界。
