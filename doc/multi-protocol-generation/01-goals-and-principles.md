# 01. 目标与设计原则

## 1. 背景

当前系统已经存在生成请求、生成事件、Provider、Pipeline、Controller 和响应 Sink 等组件。下一步不是简单增加一个新接口，而是把这些组件整理为稳定的边界，使未来增加新协议时不会持续污染核心代码。

当前实现以 OpenAI-compatible 为第一协议完成了这次边界迁移；Claude 协议明确延后，不作为本轮完成标准。

## 2. 目标

### 2.1 覆盖当前生成能力

现有接口必须继续支持：

- 非流式生成；
- 流式生成；
- 多轮消息；
- 图片输入；
- 工具调用和工具结果；
- 会话连续性；
- Provider 选择、重试和错误处理。

### 2.2 支持协议扩展

新协议的主要新增内容应限制为：

- 路由入口；
- 请求适配器；
- 响应编码器；
- 工具和能力映射；
- 协议错误格式化；
- 协议测试。

新增协议不应要求复制 `GenerationService`、`GenerationPipeline` 或 Provider 实现。

## 3. 非目标

1. 不把所有厂商字段强行塞入统一模型。
2. 不在核心层直接处理某个协议的 JSON 或 SSE。
3. 不通过大量 `if/else` 支持新协议。
4. 不承诺所有协议能力都能无损互相转换。
5. 不使用协议名称伪造上游模型能力。

## 4. 设计原则

### 单向依赖

依赖方向必须保持：

```text
Transport → Protocol Adapter → Unified Contract → Application → Provider
                                                       ↓
                                                Unified Events → Sink
```

核心层不得反向依赖 Controller、HTTP 请求对象或协议编码器。

生产链路中的 Sink 选择由 `ProtocolRegistry` 返回的协议 Sink 工厂完成。具体
OpenAI Sink 仍使用 Drogon transport 类型实现，但 Controller 不再直接构造它们。

### 语义优先

统一模型描述生成语义，例如文本、图片、工具调用、工具结果和停止原因，而不是描述某个协议的字段名。

### 边界翻译

所有协议差异必须在边界翻译完成：

```text
外部协议 → Unified Request
Unified Event → 外部协议
```

### 能力透明

如果协议、模型或 Provider 不支持某项能力，应明确拒绝、降级并记录，不能静默伪造。

实际可用能力为：

```text
protocol capabilities ∩ declared model capabilities ∩ provider capabilities
```

未知模型使用明确的默认能力集合，并通过 `modelCapabilitiesDeclared=false`
与已声明模型区分；并行工具在 Provider 不支持时降为串行并记录 typed degradation。

### 单一事实来源

`ContentBlock` 是消息内容的唯一内部表示。协议 Adapter 直接构造它，协议 Sink
直接消费统一事件；核心层不存在字段投影或替代请求入口。

### 可替换性

每个协议模块、Provider 和 Sink 都应可以独立替换和测试。
