# P7-W2 Account workflows 重写（完成记录）

## 输入与范围

- P1 的 characterization、P4 的可中断 worker / deadline join，以及 P5 的 store、config、workspace
  显式注入已经锁定原有 Account 管理行为和生命周期；
- 原 `accountManager.cpp` 同时承载内存索引和轮换、持久化状态转换、Chayns 登录/注册轮询、token 刷新、
  过期清理、Retool provision 分派和四条后台线程，无法按失败边界维护；
- 本项保留 `IAccountCatalog`、`IAccountAdminCommands`、`IAccountSelector` 和既有 controller/provider
  调用契约，不改账号表 schema、上游 wire protocol 或自动化配置键；
- P7-W1 已完成 Generation pipeline，本项只完成阶段 7 的 Account workflow 拆解，不提前执行 P8 的
  legacy target 删除或过渡表示清理。

## 设计与调用图

`AccountManager.cpp` 缩为约 300 行的 composition/configuration facade：注入 ports、Null fallback、
自动化设置加载、store 初始化、账号加载和 worker 启动顺序。原流程被按状态和副作用边界迁为
`aiapi_application` 的专职 source：

```text
AccountManager::init
  -> store/schema + automation settings + loadAccount
  -> AccountSelector + AccountSelectionPolicy
       -> accountList / pool rebuild / rotation / free-pro eligibility
  -> AccountRegistrationWorkflow
       -> AccountRegistrationStateMachine
            waiting -> registering -> active
            failed edge: waiting -> deleteWaitingAccount
       -> Chayns register-and-login poll | Retool provision dispatch
  -> AccountTokenWorkflow
       -> validate / login refresh / queued refresh / reachable retry
  -> AccountHealthWorkflow
       -> channel quota / expiry cleanup / upstream deletion / type scan
  -> AccountWorkers
       -> interruptible waits + deadline-aware join
```

- `AccountSelectionPolicy` 是不访问 HTTP/store 的纯规则；`AccountSelector` 独占双索引、heap 重建、
  轮换和 CRUD。Pro 候选除 token/active 条件外必须有 workspace binding，调用方传入的排除用户名不会
  回流到池内选择。
- `AccountRegistrationStateMachine` 独占 durable `waiting → registering → active` 转换和 in-flight ID
  集合。`begin()` 若不能进入 `registering`，以及 registration workflow 后续任一失败，均先写回
  `waiting` 再调用状态受限的 `deleteWaitingAccount`；RAII scope 最终释放 in-flight 标记。
- token、registration 与 health source 各自保留原 HTTP/config/store orchestration，避免以一个
  "legacy workflow" forwarding 层继续共享业务实现。`AccountWorkflowSupport` 承担 URL、JSON envelope、
  automation-config 和生命周期筛选等共享边界规则。
- `AccountWorkers` 独占四条线程的启动/幂等 stop。长等待用 condition variable 打断，有限 shutdown
  使用 `platform::joinUntil`，不把 thread lifecycle 混回 workflow 或 selector。

## HTTP / clock 边界

`AccountHttpTransport.cpp` 和 `AccountClock.cpp` 继续属于 `aiapi_infrastructure`。workflow 只依赖
`IAccountHttpTransport` / `IAccountClock`；若把 concrete Drogon transport/real clock 移入 application，
会制造 `aiapi_application → aiapi_infrastructure` 的反向内部 target edge。

`runtime/AppWiring` 在 `AccountManager::init()` 前显式调用
`setHttpTransport(makeDrogonAccountHttpTransport())` 和 `setClock(makeRealAccountClock())`。未接线的局部
fixture 使用明确的 Null adapter，而不隐式寻找 infrastructure singleton。`check_startup_wiring.py` 和
`check_account_services.py` 固定这两个 production injection 点及其时序。

## 测试与门禁

新增 `test_account_workflow_stages.cpp`：

- `AccountWorkflow_StateMachineRollbackRestoresWaitingThenDeletes`；
- `AccountWorkflow_StateMachineTransitionFailureRollsBackReservation`；
- `AccountWorkflow_SelectorAppliesBindingRequirementAndRotationFilter`；
- `AccountWorkflow_SupportParsesWorkflowEndpointsAndEnvelopes`。

既有 fake-HTTP lifecycle tests 继续锁定 registration 失败回滚、成功 activate/load 与 token 失效清池；
worker tests 继续锁定长等待可打断、幂等 stop 与连通性重试可立即停止。

新增 `tools/arch/check_account_workflow_slice.py` 并接入 CI。它检查：

1. Account workflow closure 只由 `aiapi_application` 拥有，且不回到 `aiapi_legacy`；
2. core `accountManager.cpp` 不重新实现 selector、token、registration、health 或 worker；
3. 每个 stage 的入口、state-machine contract、rollback 的 `waiting` 在 `deleteWaitingAccount` 之前；
4. 纯 stage、真实 lifecycle 和 worker 回归均已登记。

`--selftest` 只在内存中把 rollback 的 `AccountStatus::WAITING` 改为 `ACTIVE`，并要求同一判据以 rc=4
拒绝该突变，不写工作树。

既有 `check_shutdown_deadline.py` 同步把账号 worker 的检查 owner 从现已精简的
`accountManager.cpp` 改为 `AccountWorkers.cpp`，继续验证同一组 completion signal 和
`platform::joinUntil`，而不要求恢复被删除的单文件实现。

`accountManager` 也已不再直接 include `dbManager`；db include ratchet 将其从历史白名单移入
`must_stay_clean`，使后续任何 Account source 的 concrete DB 直连都以 rc=4 失败。

## 验证结果

```text
cmake -S . -B build                              PASS
cmake --build build -j1                          PASS
ctest --test-dir build --output-on-failure       PASS (397/397)
build/src/test/aiapi_test                        PASS (397 cases / 2080 assertions)
python3 tools/arch/check_account_workflow_slice.py [--selftest]
                                                   PASS
python3 tools/arch/check_startup_wiring.py       PASS
python3 tools/arch/check_account_services.py     PASS
python3 tools/arch/check_target_layers.py        PASS (legacy_sources=19)
python3 tools/arch/check_source_ownership.py --compile-commands build/compile_commands.json
                                                   PASS
python3 tools/arch/check_test_registration.py --require-strict
                                                   PASS (397 declared / 397 registered)
python3 tools/architecture_audit.py --baseline doc/adr/audits/audit-baseline.json
                                                   PASS
```

完整 cycle/layer/db/include/enqueue/AppContext/deadline、P5 injection、P6 Provider、P7 Generation gate 及其
selftest 也在收口前重跑。架构审计为 `R1=0`、`R2=38`、`R3=6 / 2563 lines`；`aiapi_legacy` source count
由 P7-W1 后的 20 降至 19。

## 遗留、下一步与回滚

- application 外缘仍有 JsonCpp/Drogon compatibility，`aiapi_legacy` 仍是 P3 过渡 target；两者均由
  P8-W1 统一清理，不能在本项中伪造 target 依赖方向；
- 下一项为 **P8-W1 过渡代码和 debt 清理**，包括 `--require-no-legacy`、干净发布基线与完整发布验证；
- 本项没有数据 schema、配置键或上游协议迁移。回滚必须整体撤回 workflow source owner、CMake list、
  AppWiring adapter injection、P7 gate/CI、测试和文档；不得仅把某一流程拷回 core facade 或 legacy，
  否则会恢复双实现并破坏 source-owner ratchet。
