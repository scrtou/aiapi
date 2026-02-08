# sessionManager 模块说明

## 目录定位

`sessionManager` 负责“请求适配 → 会话连续性决策 → 生成编排 → 工具调用桥接 → 事件输出”的主链路。

当前已按职责拆分为四层：

- `contracts/`：跨层共享的数据契约（请求、事件、输出接口）
- `core/`：核心编排与会话状态管理
- `continuity/`：会话连续性与索引
- `tooling/`：工具调用桥接、校验、规范化、编码等能力

## 依赖方向（建议保持单向）

建议依赖关系如下：

`contracts` <- `core` <- (`continuity`, `tooling`)

同时：

- `continuity` 可依赖 `contracts` 与 `core` 中的必要会话结构
- `tooling` 可依赖 `contracts` 与 `core` 中的必要会话结构
- `contracts` 不应反向依赖其他子目录

## 目录快速入口

- 请求入口适配：`core/RequestAdapters.*`
- 会话状态与生命周期：`core/Session.*`
- 主编排服务：`core/GenerationService.*`
- 连续性策略：`continuity/ContinuityResolver.*`
- 工具调用桥接链路：`tooling/ToolCallBridge.*`、`tooling/XmlTagToolCallCodec.*`

## 新增代码建议

1. 先判断是否属于“数据契约”再写到 `contracts`。
2. 业务编排逻辑优先进入 `core`，避免分散到控制器。
3. 与工具调用格式、校验、桥接相关的逻辑统一放在 `tooling`。
4. 与会话 ID、续聊决策、响应索引相关的逻辑统一放在 `continuity`。

## include 规范（推荐）

统一使用工程绝对相对路径风格，示例：

- `#include <sessionManager/core/Session.h>`
- `#include <sessionManager/contracts/GenerationRequest.h>`
- `#include <sessionManager/tooling/ToolCallBridge.h>`
