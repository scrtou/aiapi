# 会话连续性重构开发计划（Development Plan）

本计划用于将《重构设计方案》落地为代码变更。重点：结构优雅、职责边界清晰、低耦合；允许较大改动，做好中文注释；测试由你侧充分覆盖。

---

## 1. 里程碑与交付物

### M0：规则冻结（1 次短确认）
交付：
- 确认并写死“previous_response_id 未命中时 sessionId 策略”：已确认采用 **生成新的 `sess_<uuid>`**
- 确认 ResponseIndex 清理策略（maxEntries + maxAge）默认值

验收：
- README 决策项补全


### M1：新增模块（可独立单测）
交付：
- `sessionManager/ResponseIndex.{h,cpp}`
- `sessionManager/ContinuityResolver.{h,cpp}`
- `sessionManager/TextExtractor`（或工具函数文件）

验收：
- 编译通过
- 最小单测/自测：
  - ResponseIndex bind/get/cleanup
  - ContinuityResolver：
    - responses + previous_response_id hit/miss
    - zeroWidth decode ok/miss
    - hash path 调用旧逻辑（结果一致）


### M2：Adapter 收敛请求结构
交付：
- `GenerationRequest` 新增：
  - endpointType（Chat/Responses）
  - previousResponseId（optional）
- `RequestAdapters`：
  - responses adapter 解析 previous_response_id
  - 抽取 input/messages/input_items 的文本集合

验收：
- 对同一请求，TextExtractor 输出稳定可预测


### M3：GenerationService 主流程接入
交付：
- GenerationService：
  - 开头调用 ContinuityResolver 得到 sessionId
  - Session 全局 map 用 sessionId getOrCreate
  - gate key 统一为 sessionId
  - responses：生成 responseId 并 **Early bind** 到 ResponseIndex
  - Completed：Late commit（session history/hash/lastResponseId/time）

验收：
- 两轮 responses 续接通过
- SSE 快速续接通过


### M4：Sinks/Controller 输出契约确认
交付：
- ResponsesJsonSink/ResponsesSseSink：确保 `id` 总能输出

验收：
- responses 的 JSON 与 SSE 均能在早期拿到 responseId

---

## 2. 任务拆分（Todo）

### 2.1 代码结构
- [ ] 新增：`aiapi/src/sessionManager/ResponseIndex.h/.cpp`
- [ ] 新增：`aiapi/src/sessionManager/ContinuityResolver.h/.cpp`
- [ ] 新增：`aiapi/src/sessionManager/TextExtractor.h/.cpp`（或 `SessionTextUtils.*`）

### 2.2 接口改造
- [ ] 修改：`GenerationRequest.h` 增加 endpointType、previousResponseId
- [ ] 修改：`RequestAdapters.cpp` 负责填充上述字段

### 2.3 主流程改造
- [ ] 修改：`GenerationService.cpp` 接入 ContinuityResolver
- [ ] 修改：`Session.cpp/Session.h`（必要时）补齐 session_st 字段语义（sessionId/lastResponseId 等）
- [ ] Gate key：统一 sessionId

### 2.4 输出层
- [ ] 修改：`ResponsesJsonSink.*`、`ResponsesSseSink.*` 确保 id 输出

---

## 3. 关键风险点与规避

1) **Early bind 时机太晚**导致 SSE 场景第二轮 miss
- 规避：在 Started 事件 emit 前就 bind

2) **Hash 规则误改**导致历史会话全部断链
- 规避：Hash 计算逻辑完全复用旧函数；加回归对比日志/测试

3) **ZeroWidth 文本来源不全**导致某些客户端断链
- 规避：TextExtractor 覆盖 input + messages + input_items；并取最后出现的零宽 id

4) **ResponseIndex 清理策略过激**导致续接不稳定
- 规避：默认 maxAge 足够长（例如 1h/6h），maxEntries 足够大；清理只在插入或定时触发

---

## 4. 验收 Checklist（最终）
- [ ] Chat：ZeroWidth/Hash 两模式均正常，Hash 结果不变
- [ ] Responses：
  - [ ] 每次返回 `id`
  - [ ] previous_response_id 优先续接
  - [ ] previous_response_id miss -> 新会话（符合选定策略）
  - [ ] 支持 input/messages/input_items 的 ZeroWidth 解析
  - [ ] SSE 快速续接稳定（验证 Early bind）
- [ ] Session 全局 map key 仍为 sessionId
- [ ] 关键日志具备：source/sessionId/responseId/hit-miss

