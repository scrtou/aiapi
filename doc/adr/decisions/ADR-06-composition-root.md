# ADR-06 单例改为组合根注入，一次性替换、不做兼容层

| 项 | 值 |
|---|---|
| 状态 | 已接受，待实施 |
| 来源 | RFC-001 v2.5 §2（原行 305~349），P4 拆分外移 |
| 迁移落点 | 见 [`migration-plan.md`](../migration-plan.md) |
| 数字真值源 | [`architecture-baseline.md`](../architecture-baseline.md) |

---

## 决策与理由

每个模块迁移时直接改完所有调用点，**不保留** `getInstance()` 兼容壳。

**理由**：保留兼容壳正是上一轮重构半途而废的机制（见 §0.3）。

##### v1.6 补充一：组合根无需从零设计，`main.cc` 已是雏形

实测 `main.cc:235-241` 的初始化序列**已经是显式、集中、有序**的：

```cpp
ChannelManager::getInstance().init();
AccountManager::getInstance().init();
RetoolWorkspaceManager::getInstance().init();
ApiManager::getInstance().init();
metrics::ErrorStatsService::getInstance().init(statsConfig);
```

依赖顺序现成、无循环、无隐藏初始化。`AppContext` 的构建**本质上是把这段已有顺序换一种写法**：

```cpp
// 之前
AccountManager::getInstance().init();
// 之后
ctx.accountManager = std::make_shared<AccountManager>(ctx.accountDb, ctx.channelMgr);
```

> **结论修正**：阶段 2 的难点**不在组合根设计**（原方案高估），而在 180 处调用点的机械替换 —— 是体力活，不是设计活。

##### v1.6 补充二：单例分三类，处理方式必须区分

原方案默认「全部注入」。实测 21 个类性质差异极大：

| 类别 | 特征 | 代表 | 处理方式 |
|------|------|------|----------|
| **A 类** | 持有可变状态，**部分带后台线程** | `chatSession`（`session_map` / `context_map` / `mutex_` / `clearExpiredThread_`）、`ApiManager`（优先队列 + 映射表）、`chaynsThreadReaper`、`AccountManager` | 真注入，**必须配套显式 `shutdown()`** |
| **B 类** | 仅持有一个 db 指针 | `ChannelManager`（唯一成员 `shared_ptr<ChannelDbManager>`） | 退化为普通对象，构造成本近零 |
| **C 类** | 实质无状态，方法直通 db | `ManagedAccountService`（private 段几乎为空） | 改自由函数，单例身份纯属惯性 |

**A 类的关键风险**：`chatSession` 持有 `std::thread clearExpiredThread_` 成员。
注入化会改变析构时序 —— 进程退出时若线程晚于 `session_map` 销毁，将访问已析构对象。
因此 A 类迁移**必须先补 `shutdown()` 显式停线程，再改所有权**，顺序不可颠倒。

---

---

## 补充条款（P5 补齐，v2.6）

原 ADR-06 只说了「单例改组合根注入、一次性替换、不做兼容层」，但**没说哪些单例先动、AppContext 的构造/析构次序是什么、哪些改动会踩到停机时序**。以下基于全量实测补齐。

### 6.1 实测：单例全量清单（23 个访问器）

> 上一版审计只统计了 `instance()` 写法，**漏掉了 `getInstance()` 与 `shared_ptr` 工厂写法**。
> 本表为修正后的全量清单，数字以此为准。

**A 类 —— 持有线程或后台生命周期，必须先补停机、再改所有权（5 个）**

| 类 | 位置 | 线程 | 现有停机能力 | 调用点 |
|---|---|---|---|---:|
| `BackgroundTaskQueue` | `utils/` | 工作线程池 | 有 `shutdown()` | 29 |
| `chaynsThreadReaper` | `apipoint/chaynsapi/` | `worker_` | 有 `start()`/`stop()` | 3 |
| `ErrorStatsService` | `metrics/` | `workerThread_` | 有 `shutdown()` | — |
| `AccountManager` | `accountManager/` | **4 个** | 有 `stopBackgroundThreads()`（N4 已由 detach 改 join） | 9 |
| `chatSession` | `sessionManager/core/` | 会话过期清理线程 | 有 `stopClearExpiredSession()` | — |

> `chatSession::getInstance()` 返回**裸指针**，是全清单中唯一非引用/非 shared_ptr 的访问器，改造时需额外确认无外部持有。

**A 类的关键结论**：这 5 个**都已有停机方法**（N4/N5 的成果），所以 ADR-06 原文「A 类先补 `shutdown()` 再改所有权」这一前置步骤**实际已经完成**。改所有权时唯一要保的是：`AppContext` 析构次序必须**逐字复刻 `main.cc` 现有停机顺序**，不得凭「依赖关系」重排。

**B 类 —— 无线程、有状态，可直接注入（7 个）**

`ResponseIndex`(44) · `RetoolWorkspaceManager`(22) · `ChannelManager`(12) · `ApiFactory`(5) · `ManagedAccountService`(5) · `RetoolWorkspaceService`(3) · `ApiManager`

> `ResponseIndex` **44 处**、`RetoolWorkspaceManager` **22 处**是 B 类最重的两个，单独成批。
> `RetoolWorkspace*` 若随**阶段 0.5 provider 下线**一并移除，本批直接缩小 —— **先做 0.5，再重新计数**。

**C 类 —— `shared_ptr` 工厂型 DbManager，改造模式统一（9 个）**

`SessionDbManager`(12) · `AccountDbManager`(9) · `RetoolWorkspaceDbManager`(8) · `chaynsThreadDbManager`(7) · `ConfigDbManager`(5) · `AccountBackupDbManager`(4) · `ChannelDbManager` · `ErrorStatsDbManager` · `StatusDbManager`

> C 类共性：全是 `static std::shared_ptr<X> getInstance()`，无线程，只包 DB 访问。
> 改造模式**完全一致**（构造时注入 DB 客户端，由 `AppContext` 持有），可批量机械替换，风险最低。

**另需归类**：`ErrorStatsConfig`(1) 与 `SessionExecutionGate` 按 B 类处置。

**误报剔除**：上一轮汇总表中的 `ApiInfo` / `QueryParams` / `StatusBucket` / `Channelinfo_st` / `CancellationToken` / `RequestCompletedData` / `session_st` 是**同文件内其它结构体被正则误取为类名**，不是单例，不计入。汇总表首行 `AccountDbManager` 对应 `accountManager.h` 亦为同一误取，真实类名是 `AccountManager`。

### 6.2 `AppContext` 构造与析构次序

**构造次序**（先建被依赖者）：

1. 配置与日志（`ErrorStatsConfig`）
2. DB 连接 + C 类全部 DbManager
3. B 类无线程服务
4. A 类后台设施：`BackgroundTaskQueue.start()` → `ErrorStatsService` → `AccountManager` 后台线程 → `chaynsThreadReaper.start()`
5. transport 注册（Drogon controller）

**析构次序 —— 逐字复刻 `main.cc` 现有顺序，理由已写在源码注释中，不得重排**：

| 序 | 动作 | 为什么是这个位置（源码注释中的原有理由） |
|---|---|---|
| 1 | `chaynsThreadReaper::stop()` | **不是依赖关系**，而是停机窗口：单轮要对最多 `batchLimit` 个上游线程逐个发 HTTP DELETE 并按 `deleteSpacingMs` 限速，一轮可能耗时数分钟。必须**最先发起**，让网络 IO 与后续步骤重叠收敛；否则总停机时间线性叠加，超出 SIGTERM 宽限期（通常 30s）会被 SIGKILL |
| 2 | `AccountManager::stopBackgroundThreads()` | **必须早于队列 shutdown**：账号线程（`checkToken` / `cleanExpiredAccounts`）会向队列投递任务；队列先关会 fail-fast 拒收，任务静默丢失且刷屏拒收日志 |
| 3 | `chatSession::stopClearExpiredSession()` | 该线程每轮同步删除 DB 中过期快照，**必须在 DB 设施拆除前**干净退出 |
| 4 | `BackgroundTaskQueue::shutdown()` | 最后关闭 —— 前三步都可能仍在投递任务 |

> **这是本 ADR 最容易做错的地方**。C++ 默认析构是「成员逆序销毁」，
> 而上表次序**并非构造次序的严格逆序**（Reaper 最后启动、却最先停止）。
> 因此 **`AppContext` 必须实现显式 `shutdown()` 按上表顺序停机，不能依赖成员析构顺序**；
> 析构函数只做「断言已 shutdown」，不做实际停机工作。

### 6.3 五批次替换次序

| 批次 | 范围 | 调用点规模 | 风险 | 前置 |
|---|---|---:|---|---|
| S1 | `AppContext` 骨架 + 显式 `shutdown()` + 停机次序单测 | 0 | 低 | 阶段 1 完成 |
| S2 | **C 类** 9 个 DbManager 批量替换 | ~45 | 低（模式统一） | S1 |
| S3 | **B 类** 轻量项（`ApiFactory`/`ManagedAccountService`/`ChannelManager`/`ErrorStatsConfig`/`SessionExecutionGate`） | ~23 | 中 | S2 |
| S4 | **B 类** 重量项（`ResponseIndex` 44 + `RetoolWorkspace*` 25） | ~69 | 中高 | S3；**若 0.5 已下线 retool，本批只剩 ResponseIndex** |
| S5 | **A 类** 5 个后台设施 | ~41 | **最高** | S4；每个单独提交、单独验停机 |

> **A 类放最后**的理由：它们的停机时序是**生产环境唯一不可回滚的东西** —— 改错了不会编译失败，
> 只会在某次 SIGTERM 时表现为任务丢失或被 SIGKILL 截断。前四批做完时组合根已跑通，S5 才有可信验证基线。

### 6.4 验收门禁

```bash
# S5 完成后：src/ 下不得再出现单例访问器（main.cc 与 AppContext 自身除外）
! grep -rnE '\b(instance|getInstance)\s*\(\s*\)' src --include='*.cpp' --include='*.cc' --include='*.h' \
    | grep -vE 'AppContext|main\.cc'
```

> **过渡期例外**：S1~S4 期间该命令必然失败，属预期，**只在 S5 完成后**才作为 CI 门禁生效。
> 不要在 S1 就加进 CI —— 那会逼着人一次提交改完 23 个单例，正是 ADR-06「一次性替换」最容易被误读的地方：
> **「不做兼容层」指的是不保留双轨 API，不是指必须一次提交改完全部单例**。

### 6.5 与其它 ADR 的关系

- 依赖 [ADR-01](./ADR-01-layered-architecture.md) / [ADR-02](./ADR-02-cmake-enforced-layering.md)：组合根在最外层，是唯一允许知道全部具体类型的地方。
- 与 [ADR-08](./ADR-08-concurrency-and-shutdown.md) **强耦合**：6.2 析构次序即 ADR-08 停机时序的落地形式；两者若冲突，以 `main.cc` 源码注释中的实测理由为准。
