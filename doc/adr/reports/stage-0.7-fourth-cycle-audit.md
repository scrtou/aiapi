# 阶段 0.7 · 疑似第四环核对报告

> 状态：**已完成**（2026-08-08）
> 触发：ADR-03 实测中发现 `metrics/ErrorStatsService.h` ↔ `dbManager/metrics/ErrorStatsDbManager.h`
> 双向依赖，提交阶段 0.7 核对「是否属已登记三环之一，若否则为第四个环」。

---

## 1. 结论先行

**原问题证伪，但连带发现更严重的问题。**

| 问题 | 结论 |
|---|---|
| 疑似第四环是否成立？ | **否**。它就是已登记的**环 2 `dbManager ↔ metrics`**，migration-plan:811 有原文，C2 已覆盖 |
| 那么阶段 0.7 是否无需变更？ | **需要，而且是重大变更** |

**全库目录级依赖环实测：不是 3 个环，是 6 条双向边、1 个 9 节点强连通分量。**
**且 C1+C2+C3 执行完毕后，这个 9 节点 SCC 一个节点都不会减少。**

---

## 2. 原问题的核对结果：证伪

migration-plan:793-812 的三环表原文已写明：

> 环 2 `dbManager ↔ metrics`：`ErrorStatsDbManager.h:11` → `metrics/ErrorEvent.h`；
> `ErrorStatsService.h:6` → `dbManager/metrics/ErrorStatsDbManager.h`

与 ADR-03 实测的两条边**逐字一致**。处方 C2（`ErrorEvent.h` 移入 `domain/model/`）已覆盖。

> **ADR-03 与 migration-plan 的相关表述需回写为「已核对，属环 2，非第四环」。**

补充取证支持 C2 的正确性：`ErrorEvent.h` 共 206 行，**无对应 `.cpp`**，仅依赖
`<string>/<chrono>/<json/json.h>/<cstdint>`，是纯数据契约 —— 确实只是放错了层。

---

## 3. 连带发现：环不是 3 个

以「头文件基名 → 所属顶层目录」为判据（**跨目录同名头文件数 = 0，判据无歧义**）
对 `src/`（排除 `test/`）做全量扫描：

### 3.1 六条双向边

| # | 双向边 | 是否已登记 |
|---|---|---|
| 1 | `apiManager ↔ apipoint` | ✅ 环 1 |
| 2 | `dbManager ↔ metrics` | ✅ 环 2 |
| 3 | `apipoint ↔ sessionManager` | ✅ 环 3 |
| 4 | **`accountManager ↔ dbManager`** | ❌ **未登记** |
| 5 | **`accountManager ↔ apipoint`** | ❌ **未登记** |
| 6 | **`retoolWorkspace ↔ dbManager`** | ❌ **未登记** |

> 疑问的答案不是「有没有第四个环」，而是 **「第四、第五、第六个环都在」**。

### 3.2 三条未登记边的逐条取证

**边 4 · `accountManager ↔ dbManager`**

| 方向 | 证据 |
|---|---|
| → | `accountManager.h:17` → `../dbManager/account/accountDbManager.h`（共 4 处）|
| ← | `accountDbManager.h:8` 、 `accountBackupDbManager.h:4` → `accountManager/accountManager.h` |

反向边的用途已查明：DB 层要的只是**数据结构**——
`addAccount(struct Accountinfo_st)` / `getAccountDBList()` → `list<Accountinfo_st>` /
`backupAccount(const Accountinfo_st&)`。

> **与 `ErrorEvent.h` 完全同病**：`Accountinfo_st` / `AccountAutomationSettings` /
> `AccountRequirement` / `AccountCompare` 是纯数据，却和 `class AccountManager`（服务）
> 挤在同一个 `accountManager.h` 里，导致 DB 层为了用几个 struct 反向依赖了整个服务层。

**边 5 · `accountManager ↔ apipoint`**

| 方向 | 证据 |
|---|---|
| → | `accountManager.h:16` → `APIinterface.h`，唯一用途是 `:195 registerAPIinterface(string, shared_ptr<APIinterface>)` |
| ← | `chaynsapi.h:3` 、 `nexosapi.cpp:3` → `accountManager/accountManager.h` |

> 正向边**只以 `shared_ptr<APIinterface>` 作参数、从不解引用** ——
> 与环 1 的 C1 是**同一个模式**，前向声明即可断，且 `Apicomn.h:9` 已有先例。

**边 6 · `retoolWorkspace ↔ dbManager`**

| 方向 | 证据 |
|---|---|
| → | `RetoolWorkspaceManager.cpp:4` → `dbManager/retoolWorkspace/RetoolWorkspaceDbManager.h` |
| ← | `RetoolWorkspaceDbManager.h:8` → `retoolWorkspace/RetoolWorkspaceInfo.h` |

`RetoolWorkspaceInfo.h`：144 行，**无 `.cpp`**，仅依赖 `<json/json.h>/<sstream>/<string>`，
`struct RetoolWorkspaceInfo`。

> **与 `ErrorEvent.h` / `ProviderResult.h` 第三次同病。** 处方相同：移入 `domain/model/`。

---

## 4. 最严重的一条：C1+C2+C3 解不开这个环

migration-plan:846 写着「3 环中的 2 个完全解开，第 3 个降为单边」。
**这个说法在目录级依赖图上不成立。**

实测各处方叠加后的强连通分量（SCC）：

| 方案 | 处方 | SCC 节点数 | 残留双向边 |
|---|---|---:|---:|
| S0 基线 | — | **9** | 6 |
| S1 | C1+C2+C3（**现方案**）| **9** | 5 |
| S2 | S1 + **C5** `RetoolWorkspaceInfo.h` → domain | **9** | 4 |
| S3 | S2 + **C6** `accountManager.h` 数据类型拆出 → domain | **3** | 2 |
| S4 | S3 + **C7** `APIinterface.h` 前向声明 | **2** | 1 |

> **S1 的 SCC 仍是 9 个节点 —— 与基线完全相同。**
> 即：按现方案执行完整个阶段 0.7，`accountManager / apiManager / apipoint / channelManager /
> dbManager / managedAccount / metrics / retoolWorkspace / sessionManager` **九个模块仍然互相缠死**。

### 后果直接命中 ADR-02

RFC-001:392 自己写明「CMake 不允许 static library 循环依赖」。
阶段 0.7 的**唯一目的**就是让阶段 1 的 target 拆分成为可能。

> **按现方案，阶段 0.7 完成后阶段 1 依然会失败。**
> 0.7 的验收标准「三个环拆除」是**局部指标**，通过了也不代表目标达成 ——
> 真正的验收标准必须是 **SCC 数 = 0**（或仅剩已知转阶段 2 的那一个）。

### 为什么会漏

三环表是**逐条人工定位**出来的（migration-plan:793「已定位到文件:行号」），
没有做全图 SCC 计算。人工看边看得很准，但**看不出九个模块合起来是一个环** ——
这正是 C4「环检测脚本化」应该做、却排在 C1~C3 之后的事。

> **C4 应当提前到 C1 之前执行。** 先有脚本给出基线 SCC，再动手拆，
> 否则拆完了才发现指标没动。

---

## 5. 阶段 0.7 修订方案

### 任务表

| 任务 | 估时 | 消除 | 状态 |
|---|---:|---|---|
| **C4** 环检测脚本化 + CI 门禁（R4）| 0.5 天 | 防回归 | **提前为第一项** |
| C1 `ApiManager.h` → 前向声明 | 0.5 天 | 边 1 | 不变 |
| C2 `ErrorEvent.h` → `domain/model/` | 0.5 天 | 边 2 | 不变 |
| C3 `ProviderResult.h` → `domain/model/` | 0.5 天 | 边 3 的形式违规 | 不变 |
| **C5** `RetoolWorkspaceInfo.h` → `domain/model/` | **0.5 天** | **边 6** | **新增** |
| **C6** `accountManager.h` 拆分：`Accountinfo_st` 等数据类型 → `domain/model/AccountData.h` | **1 天** | **边 4** | **新增** |
| **C7** `accountManager.h:16` `APIinterface.h` → 前向声明 + 下沉 `.cpp` | **0.5 天** | **边 5** | **新增** |
| **小计** | **4 天 ≈ 0.8 周** | | 原 2 天 / 0.4 周 |

**C6 是唯一需要真正动手术的一项**：`accountManager.h` 目前把 4 个数据类型和
`class AccountManager` 混在一起，需要拆成两个头文件。但**不涉及任何接口倒置**，
仍然只是「把文件放回它该在的层」。

### 修订后的验收标准

| 原 | 修订 |
|---|---|
| 三个环拆除 | **全图 SCC 数 ≤ 1，且唯一残留为 `{apipoint, sessionManager}`** |
| —— | 该残留由 `Session.cpp:6 → chaynsapi.h` 造成，是**真 DIP 违规**，
已明确转阶段 2 随「消灭单例」一并处理（migration-plan:836 原判断成立，不变）|

S4 实测结果与该标准一致：**SCC = 1，节点 = {apipoint, sessionManager}，双向边 = 1**。

---

## 6. 工期影响

| 项 | 变化 |
|---|---|
| 阶段 0.7：2 天 → 4 天 | **+0.4 周** |
| C4 提前（顺序调整，不增时） | 0 |

**阶段 0.7：0.4 → 0.8 周。总工期 10.5 → 10.9 周。**

> 这 +0.4 周不是新增范围，而是**原方案漏算的部分** —— 不补上，阶段 1 无法开工。
