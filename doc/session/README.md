# Session 文档索引

本目录用于沉淀 aiapi 的会话连续性（session continuity）相关设计与计划。

## 文档
- [会话连续性重构设计方案](./session_continuity_refactor_design.md)
- [会话连续性重构开发计划](./session_continuity_refactor_development_plan.md)

## 决策（已确认）
### previous_response_id 未命中时如何创建新会话的 sessionId？
- 采用方案 2：生成新的 `sess_<uuid>`

> 说明：由于 ResponseIndex 为纯内存，重启或清理导致的 miss 将自然降级为新会话。

### ResponseIndex 清理策略默认值
- `maxEntries`: 200000
- `maxAge`: 6h
