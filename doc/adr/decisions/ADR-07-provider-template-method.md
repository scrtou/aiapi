# ADR-07 Provider 采用模板方法 + 可组合管线

| 项 | 值 |
|---|---|
| 状态 | 已接受，待实施 |
| 来源 | RFC-001 v2.5 §2（原行 350~356），P4 拆分外移 |
| 迁移落点 | 见 [`migration-plan.md`](../migration-plan.md) |
| 数字真值源 | [`architecture-baseline.md`](../architecture-baseline.md) |

---

## 决策与理由

共性（重试、SSE 分帧、超时、错误映射、账号轮转、指标上报）下沉到基类；
差异（构造请求、鉴权、解析分片、错误映射）留纯虚函数。

---

---

## 补充条款（P5 补齐，v2.6）

原 ADR-07 只有 17 行，是八个 ADR 中最单薄的一个：说了「用模板方法收敛 provider 重复」，但**没说基类长什么样、钩子有哪些、管线分几段、错误怎么短路**。以下基于四个 provider 的实测结构补齐。

### 7.1 实测起点：已有 `APIinterface`，但它不是模板方法

`src/apipoint/APIinterface.h`（41 行）已是**纯虚接口**，8 个纯虚函数，四个 provider 全部继承：

| Provider | 实现行数 | `generate()` 的形态 |
|---|---:|---|
| `chaynsapi` | 1440 | 先调 `postChatMessage(session)` 再自行组装 |
| `nexosapi` | 1334 | 一行转发 `requestChatCompletion(session)` |
| `retoolapi` | 1352 | **直接遍历 `ChannelManager::getInstance()`** —— provider 内访问全局单例 |
| `OpenAiProvider` | 329 | 一行转发 `requestChatCompletions(session)` |

**关键判断**：`APIinterface` 是**接口**（全纯虚、无共享实现），ADR-07 要引入的 `ProviderBase` 是**模板方法基类**（`final` 骨架 + 受保护钩子）。两者**不是同一个东西，也不冲突** —— `ProviderBase : public APIinterface`，把四份实现里重复的骨架上提，纯虚接口保持不变。**不要把 ADR-07 误读成「重写 APIinterface」**。

### 7.2 共性实测：哪些阶段真重复，哪些是伪共性

关键词在四份实现中的出现分布：

| 候选阶段 | chayns | nexos | retool | openai | 判定 |
|---|---:|---:|---:|---:|---|
| `afterResponseProcess` | 1 | 1 | 1 | 1 | 四家齐备，已在接口中 |
| `classifyHttpError` | 0 | 3 | 7 | 0 | **仅两家有，且各写一份做同一件事 → 最该上提** |
| 重试（`retry`） | 5 | 3 | 0 | 5 | 三家各写各的 |
| `selectAccount` | 0 | 6 | 0 | 0 | **仅 nexos** —— 领域差异，不是共性 |
| `buildChatRequest` | 0 | 0 | 0 | 3 | 仅 openai，命名巧合 |
| `parseResponse` | 0 | 0 | 0 | 0 | **四家都没有这个命名** |

> **这张表最大的作用是划掉伪共性**。`selectAccount` / `buildChatRequest` 看着像天然钩子，
> 实测只有一家用；写进 `ProviderBase` 会**逼另外三家实现空函数**，是典型过度抽象。
> `parseResponse` 四家皆无 —— **凭经验想当然造钩子，正是模板方法最常见的失败模式**。

### 7.3 `ProviderBase` 钩子清单（只收有实测依据的项）

```
class ProviderBase : public APIinterface {
public:
    // 模板方法：骨架固定，final 禁止子类改写流程
    provider::ProviderResult generate(session_st& session) final;

protected:
    // —— 必须实现（纯虚）——
    virtual Result<Request,  Error> buildRequest(const session_st&)       = 0;
    virtual Result<Response, Error> sendRequest(const Request&)           = 0;
    virtual Result<ProviderResult, Error> parseResponse(const Response&)  = 0;

    // —— 可选覆写（有默认实现）——
    virtual Error classifyHttpError(int httpStatus, const std::string& body) const;  // 默认用 ADR-05 §5.2 映射表
    virtual bool  shouldRetry(const Error&, int attempt) const;                      // 默认：网络错误 / 429 / 5xx
    virtual void  onBeforeSend(Request&)    {}
    virtual void  onAfterReceive(Response&) {}
};
```

**为什么这三个是纯虚**：`buildRequest` / `sendRequest` / `parseResponse` 是四家**行为上都存在**、只是命名各异并被揉进单个函数的三段 —— nexos 的 `requestChatCompletion`、openai 的 `requestChatCompletions`、retool 的 `requestWorkflow`/`requestAgent` 都是这个揉合体。模板方法要做的就是把它拆开。**注意 7.2 说「`parseResponse` 四家皆无」指的是没有这个命名的独立函数，不是没有这个行为**（retool 实际有 `parseJsonResponse`）。

**为什么 `classifyHttpError` 给默认实现而非纯虚**：nexos 与 retool 已各写一份（3 处 / 7 处），逻辑高度相似；chayns 与 openai 根本没写。给默认实现后，**没写的两家立刻获得统一分类**，写过的两家可覆写保留差异。若设纯虚，反而要给 chayns/openai 补两份空实现。

> **本节全部签名的状态：均未实现**（P7-2 实测）。`ProviderBase` / `shouldRetry` /
> `classifyHttpError` / `onBeforeSend` / `onAfterReceive` 在 `src/` 下命中数均为 0 ——
> 这是**设计草案**，不是对现状的描述。阅读时勿将其误认为既有代码。
>
> 其中 `shouldRetry` 的落地范围已在 [migration-plan · 阶段 3 第 3.5 项](../migration-plan.md) 界定：
> **仅收敛三家 provider 的传输层重试**；`GenerationService` 的 toolcall 协议重发（7 处）与
> `accountManager` 的可达性探测（7 处）属语义层重发，**明确排除**。

### 7.4 管线阶段与错误短路

```
generate(session)  [final]
  1. buildRequest       ──Err──┐
  2. onBeforeSend              │
  3. sendRequest        ──Err──┤  短路：任一阶段返回 Err 立即跳出，
  4. classifyHttpError         ├→ 不执行后续阶段，统一在骨架末尾
  5. onAfterReceive            │  上报一次 ErrorEvent 后返回
  6. parseResponse      ──Err──┤
  7. shouldRetry ? 回到 2 ─────┘
```

**三条硬约束**：

1. **任一阶段返回 `Err` 即终止管线**，不得「记录后继续」。这是与现状差别最大处 —— 现状 90 处 `catch(std::exception)` 中相当一部分是吞掉继续跑。
2. **`ErrorEvent` 只在骨架里上报一次**，钩子内**禁止**自行上报，否则同一错误被重复计数（`Domain::UPSTREAM`）。
3. **重试只在 `sendRequest` 阶段生效**；`buildRequest` / `parseResponse` 失败**不重试** —— 确定性失败，重试只放大延迟。

### 7.5 迁移次序与一个前置判断

| 批次 | 动作 | 说明 |
|---|---|---|
| P1 | 引入 `ProviderBase`，**不改任何现有 provider** | 新增文件，零风险 |
| P2 | `OpenAiProvider`（**329 行，最小**）先迁 | 样板，验证钩子划分够不够用 |
| P3 | `nexosapi`（1334 行） | 已有 `classifyHttpError`，路径最清晰 |
| P4 | `chaynsapi`（1440 行，最复杂） | 有 `postChatMessage` 中间层，需先理清 |
| P5 | `retoolapi`（1352 行） | **视阶段 0.5 决定** |

> **P2 选 OpenAiProvider 而非按字母序**：329 行、`generate()` 仅一行转发，是唯一能在一次提交内迁完并看清「钩子够不够用」的样本。
> **若 P2 就发现三个纯虚钩子撑不住，应回头改 7.3 的划分，而不是给基类硬加钩子。**

> **`retoolapi` 的前置判断**：其 `generate()` 直接遍历 `ChannelManager::getInstance()`，属 ADR-06 的 B 类单例调用（22 处之一）。
> 若阶段 0.5 将 retool provider 下线，**P5 整批消失**；若不下线，P5 必须排在 ADR-06 的 S3 之后，否则要改两遍。**先定 0.5 的去留，再排 P5。**

### 7.6 与其它 ADR 的关系

- 使用 [ADR-05](./ADR-05-result-type.md)：全部钩子返回 `Result<T, Error>`；7.4 的短路即 `Result` 的错误传播。
- 依赖 [ADR-06](./ADR-06-composition-root.md)：`retoolapi` 的 `ChannelManager` 单例需先由组合根注入。
- 依赖 [ADR-01](./ADR-01-layered-architecture.md)：`ProviderBase` 属 infrastructure 层，是 ADR-05 §5.3「异常转 Error」边界的**主要落点**。
