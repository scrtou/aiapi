# 06. 实施计划

状态说明：`已完成` 表示代码和定向测试已落地；`部分完成` 表示不阻塞下一协议但仍有
运维完善项；`延后` 表示不属于本轮。

## 阶段 0：确认现状和建立基线（已完成）

### 任务

- 盘点 `GenerationRequest`、`GenerationEvent` 的生产者和消费者；
- 盘点 Controller、Adapter、Service、Pipeline、Provider、Sink 依赖；
- 搜索核心层中的 OpenAI/具体协议字段；
- 固定现有接口的文本、流式、图片和工具调用样例；
- 建立回归测试和可回滚提交点。

### 完成标准

现有接口测试通过，并明确协议专属逻辑的现存位置。

## 阶段 1：建立统一合同层（已完成）

### 任务

- 新增协议模块描述和操作注册；
- 新增统一内容块；
- 新增强类型工具定义、工具选择和工具结果；
- 统一错误、用量和生成事件；
- 删除消息和请求入口的重复表示。

### 完成标准

统一模型可以表达现有接口的文本、图片和工具调用语义。

实施说明：`ContentBlock` 是消息内容的唯一事实来源；Adapter 直接构造 blocks，核心
不保留镜像字段或转换分支。

## 阶段 2：建立协议模块和注册机制（已完成）

### 任务

- 实现 `IProtocolModule`；
- 实现 `IProtocolRequestAdapter`；
- 实现 `IProtocolResponseSinkFactory`；
- 实现 `ProtocolRegistry`；
- 将现有接口注册为第一个协议模块；
- 增加启动时重复注册和未知路由检查。

### 完成标准

核心流程可以通过 Registry 获得 Adapter 和 Sink，不依赖协议条件分支。

实施说明：Dispatcher 一次返回适配结果、operation、Sink factory 和协议能力，生产
Use Case 不再二次解析路由或操作。

## 阶段 3：解耦核心生成流程（已完成）

### 任务

- 清理 `GenerationService` 中的协议判断；
- 清理 `GenerationPipeline` 中的协议编码；
- 确认 Provider 只使用统一请求和事件；
- 将重试、连续会话和工具桥接保持在核心层；
- 增加 Provider capability check。

### 完成标准

新增协议不需要复制或修改核心生成流程。

## 阶段 4：迁移现有协议（已完成）

### 任务

- 将现有请求解析迁移到独立 Adapter；
- 将现有 JSON/SSE 输出迁移到独立 Sink；
- 将现有工具调用迁移到统一内容块；
- 保持已发布路由和响应契约；
- 每完成一个 Adapter/Sink 就运行回归测试。

### 完成标准

现有接口行为保持不变，核心层不再依赖其字段名称。

实施说明：Controller 只绑定 HTTP JSON/SSE IO，OpenAI Chat/Responses Sink 由协议
模块注入的工厂在生产任务中创建。

## 阶段 5：实现第二个协议（延后：Claude）

### 任务

- 新增独立 Controller/路由；
- 实现请求 Adapter；
- 实现非流式 Sink；
- 实现流式 Sink；
- 实现工具和能力映射；
- 实现错误格式化；
- 完成协议契约测试。

### 完成标准

第二个协议可以复用核心生成流程，并且不增加核心层协议分支。

本轮不创建 Claude 路由、Adapter 或 Sink。开始该阶段前，先以当前模拟协议验收保证
核心扩展点可用。

## 阶段 6：完善通用能力（部分完成）

### 任务

- 增加协议能力、模型能力和 Provider 能力的交集判断；
- 增加统一观测字段；
- 增加协议开关、灰度和回滚；
- 增加敏感数据脱敏；
- 增加跨协议一致性测试。

### 完成标准

新增协议具有清晰的能力边界、错误行为和运维开关。

已完成：协议/模型/Provider 能力交集、图片/流式/工具明确错误、并行工具串行降级、
模型能力声明标记、协议 metadata 脱敏。

待完成：协议级灰度开关、完整观测指标、配置化回滚和更广泛的跨协议一致性测试。

## 阶段 7：新增协议模板化（扩展性验收已完成）

### 任务

- 建立新协议目录模板；
- 建立 Adapter/Sink/Module 的脚手架；
- 建立协议契约测试模板；
- 编写新增协议操作手册；
- 验证第三个模拟协议只需修改边界层。

### 完成标准

新增第三个协议时，核心层、Provider 和 Pipeline 无需修改。

模拟协议已经验证 Module、Adapter、Sink Factory、能力映射和 route dispatch；正式
脚手架生成器仍可在接入 Claude 前补充。

## 本轮六项问题结论

1. Sink 已进入协议工厂生产链路，Controller 不再构造具体 Sink。
2. `Message::blocks` 是唯一 canonical 消息表示，不保留镜像字段。
3. 能力交集已执行，错误与降级行为明确。
4. 工具 started/delta/done 状态机已由 SSE Sink 消费并支持重放去重。
5. 路由、operation、Adapter、Sink factory 和协议能力由 Dispatcher 一次绑定。
6. 模拟协议完成边界验收；Claude 作为真正第二协议延后。

## 推荐提交顺序

```text
1. test: 固定现有协议回归基线
2. refactor: 增加统一 generation contracts
3. refactor: 增加 protocol module 和 registry
4. refactor: 解耦 generation core
5. refactor: 迁移现有 adapter 和 sink
6. feat: 增加第二协议模块
7. test: 增加跨协议和端到端测试
8. ops: 增加能力、观测、灰度和回滚
9. docs: 增加新协议开发模板
```
