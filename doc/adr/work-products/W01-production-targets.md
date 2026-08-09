# W01 · 生产 target 化设计

| 项 | 值 |
|---|---|
| 状态 | TODO |
| 前置 | W00 clean baseline、运行时覆盖报告 |
| 目标 | 删除测试 `PROJECT_SOURCES`，建立唯一 production source owner |

## Target 责任

| Target | 允许内容 | 禁止内容 |
|---|---|---|
| `aiapi_platform` | Result/Error/Deadline/Cancellation/Clock/Logger port | Drogon、DB、Provider、Controller |
| `aiapi_domain` | 纯模型、策略、port | JsonCpp、Drogon、SQL、HTTP、singleton |
| `aiapi_application` | use case、pipeline、stage、状态机 | 具体 Provider/DbManager、Drogon request/response |
| `aiapi_infrastructure` | ProviderBase、Chayns/Retool、DB store、HTTP、executor、codec | Controller 路由、业务 singleton |
| `aiapi_transport` | Controller、Filter、SSE/JSON sink、HTTP codec | DB 查询、账号池、Provider 选择策略 |
| `aiapi_runtime` | AppContext、Builder、RouteRegistrar、ShutdownCoordinator | 业务规则实现 |
| `aiapi` | `main.cc` | 业务类和具体 store/provider 的直接 new |

## 迁移检查

1. `src/test/CMakeLists.txt` 删除 `PROJECT_SOURCES`；测试改为链接最小生产 target。
2. CMake 脚本拒绝同一 `.cpp` 出现在多个 production target。
3. 生成 `compile_commands.json`，对每个 target 运行 include/layer/cycle 审计。
4. 首个闭包从 platform/domain 的纯工具开始，不把整个 legacy executable 重命名成 library。
5. 每次移动后执行 clean build、全量测试和 `nm`/link 检查，确认测试使用生产库符号。

## 回滚

target 化只改变构建图，不改变数据库 schema；失败时恢复上一版 CMake 和 target 链接，不需要数据恢复。
