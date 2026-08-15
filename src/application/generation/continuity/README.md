# continuity 目录说明

## 职责

`continuity` 负责“同一会话如何续接”的判定与索引管理。

## 当前文件

- `ContinuityResolver.*`：连续性决策（new session / previous_response_id / zero-width / hash）
- `ResponseIndex.*`：responseId 到 sessionId 的索引维护
- `TextExtractor.*`：用于连续性判定的文本提取辅助

## 维护建议

- 续聊策略变更优先修改 `ContinuityResolver`，避免把策略散落到控制器。
- 索引一致性（绑定、迁移、删除）应集中在本目录处理。
