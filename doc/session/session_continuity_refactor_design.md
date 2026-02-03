# 会话连续性重构设计方案（Chat Completions + /v1/responses）

## 1. 背景与目标

aiapi 需要同时兼容两类 OpenAI 兼容接口：
- Chat Completions API
- /v1/responses API

本次重构目标：
1) **优雅与清晰**：结构清晰、逻辑清晰、低耦合。
2) **统一主键**：`Session` 类全局 map 的 key 仍为 **sessionId**（即“会话连续模式决定的会话id”）。
3) **/v1/responses 规则**：
   - 若请求携带 `previous_response_id`，则优先使用它续接会话。
   - 若不携带，则按配置（ZeroWidth / Hash）选择会话连续模式。
   - 无论采用哪种，响应中必须包含 `response.id`。
4) **输入覆盖**：/v1/responses 支持从 `input`、`messages`、`input_items` 中解析 ZeroWidth 会话信息。
5) **Hash 规则保持现状**：不修改当前 hash 的计算规则。
6) **ResponseIndex 纯内存**：允许重启丢失；丢失后行为可预测（降级为新会话）。

非目标：
- 不引入 DB 持久化（本阶段）。
- 不更改模型调用、工具调用、鉴权等其它业务逻辑。

---

## 2. 核心概念与不变量

### 2.1 术语
- **TrackingMode**：`ZeroWidth` 或 `Hash`（由配置决定）。
- **sessionId**：会话主键，Session 全局 map key。
- **responseId**：/v1/responses 每次请求生成的响应实例 id（必须返回）。
- **previous_response_id**：/v1/responses 用于续接上一次响应的指针。
- **ResponseIndex**：内存索引 `responseId -> sessionId`。
- **ContinuityResolver**：会话连续性决策器：从请求解析出应该使用的 sessionId。

### 2.2 不变量（必须满足）
1) **Session 存取永远以 sessionId 为唯一 key**。
2) /v1/responses：**每次都生成 responseId 并返回**。
3) /v1/responses：如果携带 previous_response_id，**必须优先**用它续接（通过 ResponseIndex）。
4) 为保证 SSE/并发续接：**responseId 生成后要尽早 bind 到 ResponseIndex**。

---

## 3. 优先级规则（最核心业务规则）

### 3.1 Chat Completions
只按配置模式决定 sessionId：
- ZeroWidth：从 `messages` 中解码 sessionId；解码失败则创建新 sessionId。
- Hash：按现有 hash 规则计算 sessionId。

### 3.2 /v1/responses
优先级固定：
1) 若请求带 `previous_response_id`：
   - ResponseIndex 命中：使用命中的 sessionId
   - ResponseIndex 未命中：创建新会话（见第 4 节“降级策略”）
2) 若请求不带 `previous_response_id`：按配置（ZeroWidth / Hash）决定 sessionId。

> 这条规则使“第一轮不带 previous_response_id”不会不确定：第一轮按配置得到 sessionId；第二轮若带 previous_response_id，则通过 ResponseIndex 反查回第一轮 sessionId，从而续接。

---

## 4. ID 规范与降级策略

### 4.1 responseId（/v1/responses）
- 每次请求生成新的 `responseId`（建议 `resp_<uuid/时间戳+随机>`）。

### 4.2 sessionId
- Hash 模式：保持现有规则（原样）。
- ZeroWidth 模式：
  - 解码成功：使用解码得到的 sessionId
  - 解码失败：生成新的 `sess_<uuid>`


### 4.3 previous_response_id 未命中时的 sessionId（已确认）
- 采用方案 2：生成新的 `sess_<uuid>`

---

## 5. 模块化设计（解耦边界）

### 5.1 ContinuityResolver（新模块）
职责：只做“从请求得到会话连续性决策”，输出 sessionId，不直接操作 Session map，不做输出编码。

接口建议：
```cpp
struct ContinuityDecision {
  enum class Source { PreviousResponseId, ZeroWidth, Hash, NewSession };
  Source source;
  TrackingMode mode;
  std::string sessionId;
  std::string debug; // 可选
};

class ContinuityResolver {
public:
  ContinuityDecision resolve(const GenerationRequest& req);
};
```

依赖：
- 配置（TrackingMode）
- Hash 计算器（复用现有逻辑）
- ZeroWidth 解码器（复用/增强现有逻辑）
- ResponseIndex（只读查询 previous_response_id）

### 5.2 ResponseIndex（新模块，纯内存）
职责：提供 `previous_response_id -> sessionId` 的查询能力。

接口建议：
```cpp
class ResponseIndex {
public:
  static ResponseIndex& instance();
  bool tryGetSessionId(const std::string& responseId, std::string& outSessionId);
  void bind(const std::string& responseId, const std::string& sessionId);
  void cleanup(size_t maxEntries, std::chrono::seconds maxAge);
};
```

### 5.3 TextExtractor + ZeroWidthDecoder（纯函数/小工具）
为避免 JSON 解析散落在 GenerationService：
- TextExtractor：从 GenerationRequest 统一抽取文本集合
  - Responses：覆盖 input/messages/input_items
  - Chat：覆盖 messages
- ZeroWidthDecoder：从 texts 扫描并取“最后一次出现”的 sessionId

### 5.4 GenerationService（编排器）
职责：只编排流程：
1) ContinuityResolver.resolve(req) -> sessionId
2) Session.getOrCreate(sessionId)
3) gate（以 sessionId 为 key）
4) 调用 provider 生成
5) responses：生成 responseId 并尽早 ResponseIndex.bind
6) sink 输出
7) Completed 后更新 session（历史/哈希/lastActivityTime/lastResponseId）

### 5.5 Sinks（输出层）
职责：只做协议编码（JSON/SSE），不参与会话决策。

---

## 6. session_st 建议结构（允许调整但保持语义清晰）

原则：
- **sessionId 必须明确为主键字段**。
- 保留现有功能字段（systemPromptHash/contextHash/useTextBridge/...），避免破坏既有能力。

建议（示意）：
```cpp
struct session_st {
  // identity
  std::string sessionId;              // Session map key
  TrackingMode trackingMode;          // Hash / ZeroWidth

  // responses observability
  std::string lastResponseId;         // optional but recommended

  // existing (keep)
  std::string systemPrompt;
  std::string systemPromptHash;
  std::string contextHash;
  bool useTextBridge;
  std::chrono::steady_clock::time_point lastActivityTime;
  std::vector<Message> conversationHistory;
};
```

---

## 7. 写入时机（Early bind vs Late commit）

为兼容“回复后再转移”的习惯，同时保证 SSE 快速续接：

1) **Early bind（必须尽早）**
- responses：生成 responseId 后立即 `ResponseIndex.bind(responseId, sessionId)`
- 目的：Started 事件一出，下一轮带 previous_response_id 就能命中

2) **Late commit（可在完成后）**
- session 的 conversationHistory/contextHash/lastActivityTime/lastResponseId 等在 Completed/close 后更新

---

## 8. 输出契约

### /v1/responses
- 非流式：返回体必须包含 `id`。
- SSE：Started（或尽可能早的事件）包含 `id`。

### Chat Completions
- ZeroWidth：输出继续嵌入零宽 session 信息（保持现状）。
- Hash：不嵌入。

---

## 9. 测试矩阵（建议）

### 9.1 /v1/responses
- Round1 不带 previous_response_id：
  - Hash：sessionId 与旧规则一致；返回 responseId
  - ZeroWidth：
    - 能从 input/messages/input_items 解码
    - 解码不到生成新 sess_...
- Round2 带 previous_response_id=Round1.responseId：必须命中同 sessionId
- previous_response_id 不存在/重启丢索引：miss -> 新会话
- SSE 快速续接：Started 下发 id 后立刻发下一轮，必须命中（验证 Early bind）

### 9.2 Chat Completions
- ZeroWidth 解码续接
- Hash 可复现且不变

