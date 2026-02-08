# tooling 目录说明

## 职责

`tooling` 负责工具调用相关的能力模块，包括：

- 工具调用桥接与编解码
- 参数规范化与校验
- 严格客户端规则
- 兜底工具调用生成
- 工具定义编码与桥接辅助

## 当前文件

- `ToolCallBridge.*`：工具调用桥接主入口
- `XmlTagToolCallCodec.*`：XML 标签工具调用编解码
- `ToolCallValidator.*`：工具调用校验
- `ToolCallNormalizer.*`：工具参数规范化
- `StrictClientRules.*`：严格客户端约束处理
- `ForcedToolCallGenerator.*`：强制工具调用生成
- `ToolDefinitionEncoder.*`：工具定义编码
- `BridgeHelpers.*`：桥接公共辅助能力

## 维护建议

- 新增工具调用策略时，优先评估是否应扩展现有模块而不是新建平行逻辑。
- “解析 → 规范化 → 校验 → 降级”流程保持清晰顺序，避免跨模块交叉回调。
