# 评审第 8 条 · Result 错误模型收敛 —— 取证与裁决

> 状态：**已结案**（2026-08-07）
> 取证方式：四套错误类型定义逐一通读 + 调用链实测，非测试源文件 149 个

---

## 1. 裁决

**采纳，但绝大部分已在 ADR-05 v2.6 补充条款中落地。** 评审第 8 条要求的三件事
（`Error` 形态、转换边界、0 处铺开路径）在 ADR-05 §5.2 / §5.3 / §5.4 已有答案，
本次取证**验证其成立**，不推翻。

新增三项 v2.6 未发现的事实：

| # | 修正 | 性质 |
|---|---|---|
| M1 | ADR-05 两个实测数字有误 | 数据订正 |
| M2 | `chaynsapi::generate()` 是**伪适配器** | 结构性发现，影响阶段 3 |
| M3 | `ProviderResult` 存在**往返双写** | 结构性发现，影响 B2 批次 |

---

## 2. M1 · 实测数字订正

| 项 | ADR-05 记录 | 本次复测 | 判定 |
|---|---:|---:|---|
| 自己 `throw` | 10 | **10** | 一致 |
| `catch` | 100 | **121** | 偏低 21 |
| `std::optional` | 59 | **35** | 偏高 24 |
| `Result<` 用法 | 0 | **0** | 一致 |

偏差根因是**口径没写清**：`catch` 疑似只统计了 `.cpp`；`optional` 疑似把实现内
局部变量一起算了。

**订正口径（今后复测以此为准）**：

```
throw    : *.cpp *.cc *.h，排除 src/test/，匹配 'throw '
catch    : 同上，匹配 'catch *('
optional : 仅 *.h（只关心接口签名），排除 src/test/
Result<  : *.h *.cpp，排除 ProviderResult / GateResult 同名噪声
```

**10 : 121 是 ADR-05 §5.3 边界条款的全部依据** —— 自己抛的十处可逐个改掉，
第三方抛的一百二十一处只能在 Layer 1 出口拦住。结论不变，反而更强。

---

## 3. M2 · `chaynsapi::generate()` 是伪适配器

### 3.1 实测

| Provider | `session.response` 出现次数 |
|---|---:|
| `chaynsapi.cpp` | **24** |
| `retoolapi.cpp` | 0 |
| `nexosapi.cpp` | 0 |

`chaynsapi.cpp:249` 的 `generate()` 实际做的是：

```
result.text       = session.response.message.get("message", "")
result.statusCode = session.response.message.get("statusCode", 500)
// 再按 statusCode 反推 ProviderErrorCode
```

**它不生成结果，而是回读自己刚才用副作用写进 session 的数据，再包装成 `ProviderResult`。**
错误码也不是上游给的，是**从 HTTP 状态码反推**的。

### 3.2 为什么严重

阶段 0.5 要删掉 openai 与 nexos。删完之后：

| Provider | 结构化返回 | 阶段 0.5 后 |
|---|---|---|
| nexos | 真的返回 | **删除** |
| retool | 真的返回 | 存活 |
| chayns | **伪适配** | 存活（主力） |

**两个真正实现结构化返回的 Provider 里，有一个要被删掉**，而存活的主力路径是假的。

RFC §0.3 推断「新旧路径并存导致复杂度不降反升」—— 本次取证**证实该推断，且实况更糟**：
不是并存，是新路径**寄生在**旧路径上。

### 3.3 处置

> **阶段 3 验收标准收紧 · P8-真实化**
> `chaynsapi` 必须在请求处理过程中**直接构造** `ProviderResult`，而非从
> `session.response.message` 回读。**24 处 `session.response` 引用清零**为验收条件。
> 错误码须来自上游响应体，不得由 HTTP 状态码反推。
>
> **这不是新增工作量**，而是把阶段 3 本就要做的「Provider 归一」验收标准写实 ——
> 原「接口收窄」表述允许 chaynsapi 维持现状照样过关，属验收漏洞。

---

## 4. M3 · 往返双写

`GenerationService.cpp:474-493`：

```
:474  ProviderResult result = api->generate(session);    // 从 session 读出来的
:489  session.response.message["message"]    = result.text;       // 又写回去
:490  session.response.message["statusCode"] = result.statusCode;  // 又写回去
```

注释写着「以保持旧链路兼容」。合并 M2 看，chayns 路径的完整数据流是：

```
chaynsapi 内部逻辑
  └─ 写 session.response.message
       └─ generate() 读回来，装进 ProviderResult
            └─ GenerationService 再写回 session.response.message
                 └─ 下游读 session.response.message
```

**一次完整往返，净效果为零**，代价是两处都可能改写同一份数据，出问题无法定责。

> **B2 前置条件收紧**：`ProviderError` → `Error` 转换层不能建立在当前
> `ProviderResult` 之上 —— chayns 那份的 `error` 是反推出来的，转换它等于把
> 已失真的错误信息**再稀释一次**。**M2 必须先于 B2 完成。**

---

## 5. 两套 `ErrorCode` 合并可行性（B1 前置）

| | `core/Errors.h` | `contracts/GenerationEvent.h` |
|---|---|---|
| 枚举项 | 11（含 `None`）| 10（无 `None`）|
| 其余 10 项 | **名称与语义完全一致** | 同 |
| HTTP 映射 | `errorCodeToHttpStatus` 已有 | 无，靠注释 |

**唯一差异是 `None`。** 合并方向裁定：**以 `core::error::ErrorCode` 为准，保留 `None`** ——
它已带 `errorCodeToString` 与 `errorCodeToHttpStatus`，而 B5 要收敛的 140 处状态码
映射点正需要后者。删 `None` 会连带改 `AppError`，无谓。

> `Result<T, Error>` 落地后 `None` 自然失效（用 variant 哪一侧表达）。
> **B1 不做该清理**，避免一个批次同时改类型和改语义。

---

## 6. `AppError` 现状：铺开成本处于最低点

| 项 | 数量 |
|---|---:|
| 引用 `AppError` 的非测试文件 | **4** |
| 返回 `AppError` 的函数签名 | 12 |
| `AppError::` 工厂调用 | 4 |

仅 `Errors.h` / `GenerationService.{h,cpp}` / `AiApiController.cc`。**尚未扩散。**
这是本次取证唯一的好消息，也是 B1 应尽早启动的理由。

---

## 7. 影响汇总

| 项 | 变化 |
|---|---|
| ADR-05 结论方向 | **不变**，取证支持原判断 |
| ADR-05 §5.3 边界条款 | 不变，10:121 比 10:100 更支持该条款 |
| ADR-05 §5.4 批次 | 不变，但 **B2 增加前置条件（M2 先完成）** |
| 阶段 3 验收标准 | **收紧**：chaynsapi 24 处引用清零 |
| 工期 | **不变，维持 10.4 周** |
