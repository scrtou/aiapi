# ADR-02 / ADR-03 · include 暴露方式实测

> 状态：**已完成**（2026-08-07） 三项排查至此全部结案
> ADR-02 原文要求：「阶段 1 建 target 前，需先做一次 include path 暴露方式的实测」——本报告即为该实测

---

## 1. 结论先行

**ADR-02 的强制手段当前完全失效，但根因与 ADR-02 的预设不同。**

手段 2 假设的敌人是「全局 `include_directories()`」。实测：**仓库里一个都没有**。
真正的问题是 `target_include_directories` **被用成了全局** —— 一次性把 **28 个子目录**
全部加进搜索路径，效果与全局暴露等价。

**手段 2 无需执行**（没有对象），**但手段 1 必须补上数量约束**（见 §5）。

---

## 2. 自有 CMakeLists 只有 3 个

| 文件 | 职责 | 关键内容 |
|---|---|---|
| `CMakeLists.txt`（根）| **薄壳，16 行** | `enable_testing()` + `add_subdirectory(src)` |
| `src/CMakeLists.txt` | 唯一源清单 | `add_executable` + `target_include_directories`（:108，**28 个目录**）|
| `src/test/CMakeLists.txt` | 测试目标 | 自己的 `target_include_directories`（:96，**独立重复一份**）|

根 CMakeLists 的注释写明设计意图：源清单只维护一处，避免两个构建入口漂移。
**这个意图对源文件成立，对 include 目录不成立** —— include 目录恰恰是两边各写一份的。

### 全局 include_directories 检查结果

| 文件 | 全局 `include_directories()` | `target_include_directories` |
|---|---|---|
| 根 | **无** | 无 |
| `src/` | **无** | 有（:108）|
| `src/test/` | **无** | 有（:96）|

> ADR-02 手段 2「禁用全局 `include_directories()`」**当前无对象**。
> 保留为**防退化约束**（未来不许引入），但不计入阶段 1 工作量。

---

## 3. 真问题：`target_include_directories` 被用成了全局

`src/CMakeLists.txt:108-137` 一次列出 **28 个目录**：

```
${CMAKE_CURRENT_SOURCE_DIR}                    ← src/ 根
${CMAKE_CURRENT_SOURCE_DIR}/apiManager
${CMAKE_CURRENT_SOURCE_DIR}/apipoint
${CMAKE_CURRENT_SOURCE_DIR}/apipoint/chaynsapi
${CMAKE_CURRENT_SOURCE_DIR}/apipoint/nexosapi
...
${CMAKE_CURRENT_SOURCE_DIR}/utils
```

**后果**：任何 `.cpp` 都能用 `#include "任意头文件名.h"` 直接命中另一个模块的私有头，
无需写路径。这是 P1「约定没有强制力」的构建层根源 ——
**不是缺少约束，是构建配置主动拆掉了约束。**

### 手段 1 的条款漏洞

> 原文手段 1：「所有 include 目录仅经 `target_include_directories` 的
> `PRIVATE/PUBLIC/INTERFACE` 暴露，让越界 include 在编译期失败」

形式上仓库**已经满足**（确实只走 `target_include_directories`），但因为暴露了 28 个目录，
实际效果为零。

> **手段 1 只约束了「经由什么机制暴露」，没约束「暴露多少」。**
> 按当前表述，仓库现状可判定为合规。**这是条款漏洞，必须补数量约束。**

---

## 4. ADR-03 的真实工作量：此前未量化

### 4.1 include 写法分布（全库）

| 写法 | 数量 | 处置 |
|---|---:|---|
| 尖括号根相对 `<apipoint/...>` | **264** | 已合规，保留 |
| 双引号同目录 `"Foo.h"` | **200** | **需改写** |
| 双引号相对 `"../foo/Foo.h"` | **11** | **需改写** |

### 4.2 关键推论

**那 200 处同目录 include 之所以能编译，完全依赖 §3 的 28 个目录暴露。**

ADR-03 一旦把 include 根收敛为单一 `src/`：

> **200 处 include 立即编译失败。**

这是 ADR-03 的**真实规模，此前从未量化**。原文只给了一个示例
（`#include "domain/session/Session.h"` 而非 `"Session.h"`），读起来像风格调整，
实为一次全库机械改写。

### 4.3 可机械完成，但有前置条件

改写规则确定：文件名 → 其在 `src/` 下的唯一相对路径。前提是**头文件名全库唯一**。

> **前置检查已执行（本次）：全库 `.h` 文件名重名数 = 0。**
> 机械改写的前提成立，可直接脚本化，无需先改名。
>
> 该检查**仍应保留为阶段 1 的守门项** —— 阶段 0.5 会删 openai/nexos、阶段 1~3 会移动大量文件，
> 改写脚本执行**当天**必须重跑一次。今天为 0 不代表那天为 0。

### 4.4 那 11 处 `../` 相对 include 中的跨层信号

非测试文件仅 4 处：

| 位置 | 性质 |
|---|---|
| `accountManager.cpp:14` → `../dbManager/channel/channelDbManager.h` | **跨层**：账号管理直接摸 DB 层私有头 |
| `chaynsapi.h:5` → `../../apiManager/ApiFactory.h` | Provider 反向依赖工厂 |
| `dbManager/metrics/ErrorStatsDbManager.h:11` → `../../metrics/ErrorEvent.h` | DB 层依赖观测层 |
| `metrics/ErrorStatsService.h:6` → `../dbManager/metrics/ErrorStatsDbManager.h` | 观测层依赖 DB 层 |

**后两条构成 metrics ↔ dbManager 双向依赖。**

> **阶段 0.7 新增核对项**：确认该双向依赖是否属于已登记的三个环之一。
> **若不属于，则是第四个环**，阶段 0.7 的范围需要相应扩大。

---

## 5. 对 ADR-02 / ADR-03 的修订

### ADR-02 手段表

| # | 原表述 | 修订 |
|---|---|---|
| 1 | 仅经 `target_include_directories` 暴露 | **补数量约束：每个 target 至多 1 个 include 目录**（`src/` 根）。当前 28 个 |
| 2 | 禁用全局 `include_directories()` | **降级为防退化约束**，当前无对象，不计工作量 |
| 3 | 架构测试扫描 domain 的 `#include` | 不变 |
| **6** | *（新增）* | **CI 检查各 target 的 include 目录列表必须一致** —— `src/` 与 `src/test/` 各写一份，已漂移 |

### ADR-03 工作量

| 项 | 原 | 修订 |
|---|---|---|
| 描述 | 「include 写全相对路径」 | **明确为 211 处改写**（200 同目录 + 11 相对）|
| 前置 | 无 | **验证头文件名全库唯一** —— 本次已测，**重名数 = 0**，前提成立 |
| 方式 | 未说明 | **脚本机械改写 + 编译验证**，非人工逐个改 |
| 工期 | 未单列 | **+0.5 天** |

---

## 6. P12 补充：测试与主程序的耦合方式

`src/test/CMakeLists.txt:58-93` 用 `PROJECT_SOURCES` **逐个列举 32 个**主程序 `.cpp`，
与测试源一起 `add_executable`。即：

> **测试不链接主程序产物，而是重新编译其中一部分源文件。**

后果：

1. 测试只覆盖**被列举到的 32 个文件**，主程序源文件约 80 个 —— **其余六成源文件测试侧根本不编译**；
2. 新增源文件若不同步登记进 `PROJECT_SOURCES`，**静默漏测，无任何报错**；
3. 与根 CMakeLists 注释宣称的「源清单只维护一处」**直接矛盾** —— 实为两处。

> **阶段 1 一并解决**：五层 library target 建立后，测试改为 `target_link_libraries`
> 链接各层，**列举式 `PROJECT_SOURCES` 应当消失**。
> 这是阶段 1 建 target 的**顺带收益，不额外计工期**。

---

## 7. 工期影响

| 变动 | 天 |
|---|---|
| ADR-02 手段 2 —— 无对象，不计 | 0 |
| ADR-03 · 211 处 include 机械改写 + 编译验证 | **+0.5** |
| ADR-03 前置 · 头文件名唯一性验证 | 0（**已执行，重名数 = 0**）|
| 阶段 0.7 · 第四个环的核对 | 0（核对项）|
| P12 · `PROJECT_SOURCES` 消除 | 0（阶段 1 顺带）|
| **净变化** | **+0.5 天 ≈ +0.1 周** |

**总工期 10.4 → 10.5 周。**
