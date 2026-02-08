# core 目录说明

## 职责

`core` 是 sessionManager 的主业务层，负责：

- 请求适配与会话物化
- 会话状态管理与执行门控
- 生成调用主流程编排
- 错误映射与统一收敛

## 当前文件

- `Session.*`：会话存储、生命周期与上下文维护
- `GenerationService.*`：主编排入口与执行流程
- `GenerationServiceEmitAndToolBridge.cpp`：结果事件发送与工具桥接相关输出处理
- `RequestAdapters.*`：协议请求转换
- `ClientOutputSanitizer.*`：客户端输出清洗
- `Errors.h`：统一错误定义
- `SessionExecutionGate.h`：并发执行门控

## 维护建议

- 与“请求生命周期”强相关的逻辑优先放在本层。
- 尽量避免在控制器中复制编排逻辑。
- 对外暴露能力应通过清晰入口函数收敛（避免散点调用）。
