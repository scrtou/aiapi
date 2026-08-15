# sessionManager 模块说明

## 目录定位

`sessionManager` 负责“请求适配 → 会话连续性决策 → 生成编排 → 工具调用桥接 → 事件输出”的主链路。

当前已按职责拆分为五层：

- `contracts/`：跨层共享的数据契约（请求、事件、输出接口）
- `core/`：核心编排与会话状态管理（不选择协议路由或具体 Sink）
- `continuity/`：会话连续性与索引
- `tooling/`：工具调用桥接、校验、规范化、编码等能力
- `protocol/`：协议模块、请求适配器、响应 Sink 工厂、能力映射与 Registry Dispatcher

## 依赖方向（建议保持单向）

建议依赖关系如下：

`contracts` <- (`protocol`, `core`) <- (`continuity`, `tooling`)

同时：

- `continuity` 可依赖 `contracts` 与 `core` 中的必要会话结构
- `tooling` 可依赖 `contracts` 与 `core` 中的必要会话结构
- `contracts` 不应反向依赖其他子目录

## 目录快速入口

- 请求入口适配：`protocol/<id>/<Id>RequestAdapter.*`，协议模块将原始请求转换为统一请求
- 会话状态与生命周期：`core/Session.*`
- 稳定生成入口：`core/GenerationService.*`（薄 facade）
- 请求/调用/提交编排：`core/GenerationPipeline.*`
- 响应、工具与事件编排：`core/GenerationResponsePipeline.*`
- 连续性策略：`continuity/ContinuityResolver.*`
- 工具调用桥接链路：`tooling/ToolCallBridge.*`、`tooling/XmlTagToolCallCodec.*`
- 协议注册入口：`protocol/common/ProtocolRegistry.*`
- 当前协议模块：`protocol/openai/OpenAiProtocolModule.*`

## 新增代码建议

1. 先判断是否属于“数据契约”再写到 `contracts`。
2. 业务编排逻辑优先进入 `core`，避免分散到控制器。
3. 与工具调用格式、校验、桥接相关的逻辑统一放在 `tooling`。
4. 与会话 ID、续聊决策、响应索引相关的逻辑统一放在 `continuity`。

5. 核心消息只使用 `Message::blocks`。协议 Adapter 直接构造 blocks，Sink 在边界编码
   统一事件；不得新增消息镜像字段。

6. 新协议必须通过 `ProtocolRegistry::dispatch()` 绑定 operation、Sink factory 和
   capability mapper；本轮不新增 Claude 实现。

## include 规范（推荐）

统一使用工程绝对相对路径风格，示例：

- `#include <application/generation/core/Session.h>`
- `#include <application/generation/contracts/GenerationRequest.h>`
- `#include <application/generation/tooling/ToolCallBridge.h>`
