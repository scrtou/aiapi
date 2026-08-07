# ADR-05 Result 类型统一，跨层禁止抛异常

| 项 | 值 |
|---|---|
| 状态 | 已接受，待实施 |
| 来源 | RFC-001 v2.5 §2（原行 272~304），P4 拆分外移 |
| 迁移落点 | 见 [`migration-plan.md`](../migration-plan.md) |
| 数字真值源 | [`architecture-baseline.md`](../architecture-baseline.md) |

---

## 决策与理由

统一为 `Result<T, Error>`，基于 `std::variant` 实现（C++17 原生可用，见 §4.5）。
现有 `ProviderResult` 收敛为 `Result<Generation>` 的特化使用。

**理由**：当前存在 `ProviderResult` / `Errors` / `ErrorEvent` / `ErrorStats` 四套并行的错误表达。

---

##### v2.3 补充：`Result` 与第三方异常的转换边界（此前缺失）

实测现状暴露了一个 ADR-05 没回答的问题：

| 项 | 实测 |
|---|---:|
| 自己 `throw` | **10** |
| `catch` | **100** |
| `Result<` 现有用法 | **0** |
| `std::optional` | 59 |
| bool + 出参风格（粗估） | 40 |

**10 个 throw 对应 100 个 catch** —— 说明九成 catch 接的不是自己抛的异常，而是 **Drogon / jsoncpp / DB 驱动**抛的。
这类异常**无法禁止**，ADR-05 的「跨层禁止抛异常」若不指定边界，最终会变成 `Result` 与 `try/catch` 两套体系并存，比现状更糟。

**补充决策**：

1. **转换边界设在 infrastructure 的出口**。第三方异常一律在 Layer 1 内捕获并转为 `Error`，**禁止外泄**到 application / domain。
2. domain / application **内部不写 `try`**；若出现，视为 Layer 1 边界没封干净，属缺陷而非风格问题。
3. `Result<T>` 从 **0 处用法**铺到全项目是一次**大规模签名变更**，与「五层 target + include 收敛」并列压在阶段 1 的 1.0 周内**不现实**。
   本版**不改工期数字**，仅标注该项为 §7 新增风险行，待阶段 1 启动前单独拆分立项。

---

---

## 补充条款（P5 补齐，v2.6）

原 ADR-05 只说了「统一 `Result<T, Error>`、跨层禁止抛异常」，但**没说 `Error` 长什么样、边界画在哪、0 处用法怎么铺开**。这三条不定，ADR-05 就是不可执行的。以下逐条补齐，全部基于实测而非设想。

### 5.1 实测：错误表达已有**四套并行体**，ADR-05 是收敛而非新建

| # | 位置 | 类型 | 成员数 | 现状定位 |
|---|---|---|---:|---|
| 1 | `sessionManager/core/Errors.h` | `error::ErrorCode` | 11 | 已含 `errorCodeToString` + `errorCodeToHttpStatus` |
| 2 | `sessionManager/contracts/GenerationEvent.h` | `ErrorCode` + `struct Error` | 10 | 与 #1 **语义几乎完全重复**，仅少 `None` |
| 3 | `apipoint/ProviderResult.h` | `provider::ProviderErrorCode` + `ProviderError` | 9 | 上游视角，含 `providerCode` / `httpStatusCode` |
| 4 | `metrics/ErrorEvent.h` | `Severity` + `Domain` + `EventType` 字符串常量 | 2+6+N | **观测视角，不是错误返回值** |

**关键判断（避免走错方向）**：

- #1 与 #2 是**同一个东西被写了两遍** —— 枚举项逐一对应（`BadRequest`/`Unauthorized`/`Forbidden`/`NotFound`/`Conflict`/`RateLimited`/`Timeout`/`ProviderError`/`Internal`/`Cancelled` 全同）。**合并，不是新造**。
- #3 是**下沉一层的领域概念**，`NetworkError` / `ServiceUnavailable` / `InvalidRequest` 在 #1 中无对应项，**不能粗暴合并**，应保留为 infrastructure 内部类型 + 一张向 #1 的映射表。
- #4 **不参与 ADR-05**。它是观测/统计通道（`Severity`×`Domain`×`EventType`），与「函数返回值怎么表达失败」正交。**误把它拉进来会把 metrics 反向耦合进 domain**。

> **结论**：`Error` = 以 #1 的 `error::ErrorCode` 为**基准**（它已有 HTTP 映射，成本最低），
> 删除 #2 的重复定义，#3 保留并在 infrastructure 出口转换，#4 完全不动。

### 5.2 `Error` 的最终形态

字段取三套的并集，逐个都有实测出处，无一凭空添加：

| 字段 | 类型 | 出处 | 为什么必须有 |
|---|---|---|---|
| `code` | `error::ErrorCode` | #1 | 分类与 HTTP 映射的唯一依据 |
| `message` | `std::string` | #1/#2/#3 三套都有 | 面向调用方的可读信息 |
| `providerCode` | `std::string` | #2/#3 | 上游原始错误码，排障必需，**丢了就无法回溯上游** |
| `httpStatus` | `int` | #3 | 上游返回的真实状态码，与 `code` 映射出的**不一定相同**，两者都要留 |
| `detail` | `std::string` | #2 | 详细上下文，仅日志用，**禁止透传给客户端** |

`ErrorCode` **不新增枚举项**，沿用 #1 的 11 项。`ProviderErrorCode` 的 3 个无对应项按下表映射：

| `ProviderErrorCode` | → `ErrorCode` | 理由 |
|---|---|---|
| `NetworkError` | `ProviderError` | 对客户端而言都是「上游不可用」，均映射 502 |
| `ServiceUnavailable` | `ProviderError` | 同上 |
| `InvalidRequest` | `BadRequest` | 上游认为请求非法 —— 但**须在日志标注是上游判定**，避免误导为本服务参数校验失败 |
| `Unknown` | `Internal` | 兜底 |

> **保真要求**：映射会丢信息，所以 `providerCode` 与 `httpStatus` 字段**必须同时填充**，
> 让「映射后的粗分类」和「上游原始信息」并存。这是 5.2 表格里 `httpStatus` 独立于 `code` 存在的唯一理由。

### 5.3 异常转换边界：一条线，可机械检查

实测异常现状：

- **catch 共 91 处**，其中 `std::exception` **90 处**、`json::parse_error` 1 处 —— 几乎全是无差别兜底。
- **自抛 10 处**，全部是 `std::runtime_error`（`Session.cpp` 4 处 OpenSSL EVP 失败、`RetoolWorkspaceService.cpp` 6 处）。

规则：

1. **边界设在 infrastructure 出口**。所有第三方异常（Drogon / jsoncpp / DB 驱动 / OpenSSL）在 infrastructure 层被 catch 并转为 `Error`，**不允许穿出 infrastructure**。
2. **domain / application 内部不写 `try`**。这两层的函数签名一律返回 `Result<T, Error>`，没有异常可catch，写了就是错的。
3. **transport 层允许一处兜底 `catch(...)`** —— 防止未预料异常打穿 Drogon 事件循环，但**必须记录并计入 `ErrorEvent`（Domain::INTERNAL）**，不得静默吞掉。
4. **自抛的 10 处 `runtime_error` 全部改为返回 `Error`**：`Session.cpp` 的 4 处 EVP 失败 → `ErrorCode::Internal`；`RetoolWorkspaceService.cpp` 的 6 处 → 视上下文映射，且该服务属 retool（历史遗留 provider），**若阶段 0.5 将其下线，这 6 处直接随之消失** —— 顺序上**先做阶段 0.5，再回头看还剩几处**。

**可机械检查的门禁**（阶段 1 起纳入 CI）：

```bash
# domain / application 层不得出现 try / catch / throw
! grep -rnE '(try|catch|throw)' src/domain src/application --include='*.h' --include='*.cpp'
# transport 层的兜底 catch 必须伴随 ErrorEvent 上报（人工 review 项，暂不自动化）
```

### 5.4 `Result<T>` 从 **0 处**铺开：分批次序与停止条件

实测 `Result<` 现有用法 **0 处** —— 这意味着铺开是**全量签名变更**，不是增量。压在阶段 1 的 1.0 周内不现实（已列入 §7 风险）。分批次序如下，**每批独立可交付、可回滚**：

| 批次 | 范围 | 依赖前提 | 规模判据 |
|---|---|---|---|
| B1 | `Result<T>` / `Error` 类型本体 + 单测 + 两套 `ErrorCode` 合并 | 无 | 新增文件，不改调用方 |
| B2 | infrastructure 出口：`ProviderError` → `Error` 转换层 | B1 | 只改 infrastructure，上层不感知 |
| B3 | domain 内部签名改造 | B2 | **最大一批**，须在 domain 有测试覆盖后进行 |
| B4 | application / 用例编排 | B3 | 跟随 domain |
| B5 | transport：`Error` → HTTP 响应的统一出口 | B4 | 收敛现有 140 处状态码映射点 |

> **停止条件（很重要）**：若 B3 开始后发现 domain 测试覆盖不足以支撑签名大改，
> **停在 B2 并保持现状**也是一个可接受的终态 —— B1+B2 已经消除了「四套错误模型互不相通」这个最大问题，
> 而 B3~B5 的收益是一致性，不是正确性。**不要为了做完而硬推 B3**。

### 5.5 与其它 ADR 的关系

- 依赖 [ADR-01](./ADR-01-layered-architecture.md)：转换边界的「infrastructure 出口」只有在分层成立后才有意义。
- 被 [ADR-07](./ADR-07-provider-template-method.md) 使用：`ProviderBase` 各钩子的返回类型即 `Result<T, Error>`。
- 与 [ADR-08](./ADR-08-concurrency-and-shutdown.md) 无冲突：`Result` 是同步返回值，ADR-08 的「Pipeline 同步签名 + 后台线程执行」正好与之相容。
