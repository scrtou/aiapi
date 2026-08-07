# 架构决策记录（ADR）索引

> 每条 ADR 独立成文，可单独修订、单独作废，不再随 RFC-001 正文一起漂移。
> RFC-001 §2 仅保留本索引的摘要表。

| 编号 | 标题 | 状态 |
|---|---|---|
| [ADR-01](./ADR-01-layered-architecture.md) | 四层架构 + 横切 platform + 依赖倒置 | 已接受 |
| [ADR-02](./ADR-02-cmake-enforced-layering.md) | CMake target 强制分层 | 已接受 |
| [ADR-03](./ADR-03-single-include-root.md) | include 路径收敛为单一根 | 已接受 |
| [ADR-04](./ADR-04-cxx17-fixed.md) | 固定 C++17，移除标准探测 | **已落地** |
| [ADR-05](./ADR-05-result-type.md) | Result 类型统一，跨层禁止抛异常 | 已接受（P5 补齐语义） |
| [ADR-06](./ADR-06-composition-root.md) | 单例改为组合根注入 | 已接受（P5 补齐语义） |
| [ADR-07](./ADR-07-provider-template-method.md) | Provider 模板方法 + 可组合管线 | 已接受（P5 补齐语义） |
| [ADR-08](./ADR-08-concurrency-and-shutdown.md) | 并发模型与停机时序 | 已接受 |
