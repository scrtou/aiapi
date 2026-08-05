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
- `BridgeProtocolCodec.*`：请求级 JSON/XML 协议选择；统一工具定义、提示、历史、重试与响应解析

## Bridge 格式配置

- `format`：全局默认值，支持 `json` / `xml`
- `format_by_channel`、`format_by_client`、`format_by_model`：请求级覆盖
- 覆盖顺序：全局 → channel → client → model
- `allow_format_fallback=false`：默认严格使用请求阶段固定的格式

## 维护建议

- 新增工具调用策略时，优先评估是否应扩展现有模块而不是新建平行逻辑。
- “解析 → 规范化 → 校验 → 降级”流程保持清晰顺序，避免跨模块交叉回调。
Last updated: August 5, 2026 06:47 AM ET
