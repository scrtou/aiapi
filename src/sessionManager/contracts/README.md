# contracts 目录说明

## 职责

`contracts` 只承载“稳定的数据与接口契约”，避免业务实现细节耦合进来。

## 当前文件

- `GenerationRequest.h`：生成请求统一模型
- `GenerationEvent.h`：统一事件定义
- `IResponseSink.h`：输出通道抽象接口

## 设计约束

- 尽量保持“轻依赖”，避免依赖 `continuity/` 与 `tooling/` 的实现细节。
- 契约变更应优先考虑兼容性，尤其是控制器与 sink 之间的接口。
