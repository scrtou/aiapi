# 当前架构决策

本目录只表达当前有效决策。历史纠错在 `../history/CHANGELOG.md` 和 git 历史中查阅，执行进度以 `../plans/migration-plan.md` 为准。

| ADR | 当前决策 | 状态 |
|---|---|---|
| [ADR-01](./ADR-01-layered-architecture.md) | 端口与适配器；domain 最终移除 JsonCpp | 迁移中 |
| [ADR-02](./ADR-02-cmake-enforced-layering.md) | CMake target DAG + 架构规则共同强制边界 | 部分落地 |
| [ADR-03](./ADR-03-single-include-root.md) | 单一 include 根与完整路径 | 已实施 |
| [ADR-04](./ADR-04-cxx17-fixed.md) | 固定 C++17 | 已实施 |
| [ADR-05](./ADR-05-result-type.md) | 跨层失败使用 Result，按垂直切片迁移 | 待实施 |
| [ADR-06](./ADR-06-composition-root.md) | 组合根替代业务 Service Locator | 迁移中 |
| [ADR-07](./ADR-07-provider-template-method.md) | 生产 Provider 继承薄 ProviderBase + 可组合策略 | 待实施 |
| [ADR-08](./ADR-08-concurrency-and-shutdown.md) | 统一 deadline、背压和安全停机 | 部分落地 |
| [ADR-09](./ADR-09-http-io-boundary.md) | HTTP/DB/轮询只在 infrastructure，取消贯穿阻塞边界 | 待实施 |
| [ADR-10](./ADR-10-domain-model-codec-boundary.md) | domain 纯模型，JSON/DB codec 在边缘 | 已落地（P3-W4，domain JsonCpp 归零） |
| [ADR-11](./ADR-11-production-test-targets.md) | 生产库唯一 source owner，正式 target strangler 迁移 | 迁移中（legacy ceiling 39） |
