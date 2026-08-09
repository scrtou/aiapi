# P3-W3 · 正式分层 target carve-out

| 项 | 值 |
|---|---|
| 状态 | DOING |
| 前置 | P3-W1 唯一 source owner；P3-W2 单一 include 根 |
| 目标 | 建立六个正式 library target，逐闭包清空并删除 `aiapi_legacy` |
| 决策 | [ADR-01](../decisions/ADR-01-layered-architecture.md)、[ADR-02](../decisions/ADR-02-cmake-enforced-layering.md)、[ADR-11](../decisions/ADR-11-production-test-targets.md) |

## 1. 目标 target DAG

```text
aiapi_platform

aiapi_domain
  └─> aiapi_platform

aiapi_application
  └─> aiapi_domain, aiapi_platform

aiapi_infrastructure
  └─> aiapi_domain, aiapi_platform, third-party IO

aiapi_transport
  └─> aiapi_application, aiapi_domain, aiapi_platform, Drogon

aiapi_runtime
  └─> aiapi_application, aiapi_infrastructure, aiapi_transport

aiapi (main.cc)
  └─> aiapi_runtime
```

图中箭头表示“左侧可依赖右侧”，不是线性层号。infrastructure 不得依赖
application；transport 不得直接访问 DB/Provider 具体实现；runtime 只组装、不实现业务规则。

## 2. 执行原则

1. 先从 `AIAPI_LEGACY_SOURCES` 和 include graph 生成每个 `.cpp/.cc` 的候选 owner；
2. 每次只 carve out 一个可编译闭包，同一提交中从 legacy 删除，禁止双 owner；
3. 每个正式 source list 使用 `AIAPI_*_SOURCES`，继续由 P3-W1 门禁检查；
4. 每个 target 只传播 P3-W2 的单一 `src/` include 根，禁止用子目录 include path 修补边界；
5. 若闭包需要上层具体类，先记录并提取窄 port，不制造反向 target link；
6. production 静态注册和 test stub 的 whole-archive/普通 archive 差异必须保真；
7. 每个闭包都运行 configure/build、定向测试、全量测试、source/include/layer/cycle 门禁和 link 证据。

## 3. 计划中的 carve-out 顺序

| 顺序 | Target | 初始闭包选择 | 状态 |
|---:|---|---|---|
| 1 | `aiapi_platform` | 无 Drogon/JsonCpp/DB 的底层结果、时钟、取消和工具 | TODO（先做依赖 inventory） |
| 2 | `aiapi_domain` | 纯模型、策略和 port；JSON 完全移除在 P3-W4 验收 | TODO |
| 3 | `aiapi_application` | Generation/Session/Account/Workspace use case 的可编译闭包 | TODO |
| 4 | `aiapi_infrastructure` | Provider、HTTP、DB store、executor、codec | TODO |
| 5 | `aiapi_transport` | Controller、filter、sink、HTTP edge codec | TODO |
| 6 | `aiapi_runtime` | composition/wiring/lifecycle，接管静态注册保真 | TODO |
| 7 | 删除 legacy | `AIAPI_LEGACY_SOURCES` 为空，主程序/测试只链接正式库 | TODO |

该表不预先伪造 source 数字。精确文件归属必须从当前 compile/include graph
生成，并在本文“过程产物”逐闭包回填。

## 4. 过程产物（随实施回填）

| 产物 | 状态 |
|---|---|
| 当前 67 个 legacy source 的 include/link/third-party inventory | TODO |
| 每个 source 的候选 target 和阻断边 | TODO |
| 每个闭包的 target diff 与 source-owner 证据 | TODO |
| 正式 target link DAG 和对应 layer-rules | TODO |
| Provider/Drogon 静态注册的最终 ownership | TODO |
| test 最小 target 链接与 stub 边界 | TODO |
| legacy 删除证据 | TODO |
| normal/coverage/ASan 与全量架构门禁 | TODO |

## 5. 退出门禁

- `aiapi_platform/domain/application/infrastructure/transport/runtime` 六个正式库存在；
- `aiapi_legacy` 和 `AIAPI_LEGACY_SOURCES` 已删除；
- 每个生产 `.cpp/.cc` owner/compile count 仍恰好为 1；
- test target 不编译生产源，只链接所需正式库；
- target link 方向与 layer rules 一致，无反向边、无新增豁免、无环；
- 422 个当前自有 include 的完整路径规则不回退，CMake 子目录 include root 为 0；
- normal/coverage/ASan 全量测试及 architecture/startup/provider-retirement 门禁通过。

## 6. 回滚

每个 carve-out 是独立可回滚闭包。失败时将该闭包的 source owner 放回
`aiapi_legacy`，恢复该 target 的 link diff，然后从空 build directory 验证。不恢复
`PROJECT_SOURCES`，不加回子目录 include root，不为了暂时编译通过而违反 target DAG。
