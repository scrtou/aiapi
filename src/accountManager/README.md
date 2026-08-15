# accountManager 模块

`AccountManager` 保持既有 controller/provider port，但 P7-W2 将可维护边界拆为独立 workflow source：

- `AccountSelector.cpp` + `AccountSelectionPolicy.*`：内存索引、池重建、轮换与 free/pro/排除规则；纯 policy 不访问 HTTP 或 store。
- `AccountRegistrationStateMachine.*`：`waiting → registering → active` 持久化转换、in-flight ID 跟踪，以及失败时固定的 `waiting → delete` 回滚顺序。
- `AccountRegistrationWorkflow.cpp`：Chayns register-and-login 轮询、激活和 Retool provision 分派。
- `AccountTokenWorkflow.cpp`：token 校验、登录刷新、待刷新队列和可中断连通性重试。
- `AccountHealthWorkflow.cpp`：配额补充、过期资源回收、上游删除和账号类型巡检。
- `AccountWorkers.cpp`：四条后台 worker 的启动、唤醒、绝对 deadline 汇合和停机。
- `AccountWorkflowSupport.*`：URL/JSON envelope/config 的共享边界辅助规则。

`AccountManager.cpp` 只保留 composition/configuration：注入 ports、Null fallback、设置加载和启动顺序。真实
HTTP/clock adapter 位于 infrastructure；`runtime/AppWiring` 必须在 `init()` 前调用
`setHttpTransport()` 和 `setClock()`。该顺序由 `check_startup_wiring.py` 固定。

`tools/arch/check_account_workflow_slice.py` 约束 source owner、stage 边界、rollback 顺序及测试覆盖；其
`--selftest` 在内存中把 rollback 的 `WAITING` 转换破坏为 `ACTIVE`，必须以 rc=4 失败。
