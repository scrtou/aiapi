# P5-W2 SessionStore / ResponseIndex / ExecutionGate 注入

## 输入

- P5-W1 已删除 Provider service locator；
- application/session 主链仍直接访问 `ResponseIndex::instance()`、
  `SessionExecutionGate::getInstance()` 与 `chatSession::getInstance()`；
- P1 的 continuity、Responses GET/DELETE、生成门控与 session transfer 测试作为行为安全网。

## 设计与调用图

```text
AppContext
  ├─ owns ResponseIndex : IResponseIndex
  ├─ owns SessionExecutionGate : IExecutionGate
  └─ wires existing chatSession owner as explicit GenerationService dependency

AiApiController / ContinuityResolver / GenerationService
  -> injected session store / response index / execution gate
```

- 新增 `IResponseIndex`、`IExecutionGate` 最小端口；
- `IResponseIndex` 以字符串承载 response JSON，JsonCpp codec 留在实现/transport，domain 端口保持净化；
- `ResponseIndex` 与 `SessionExecutionGate` 改为可构造对象，删除静态访问器；
- `ExecutionGuard` 接收 `IExecutionGate&`，RAII release 语义不变；
- `ContinuityResolver` 显式接收 index 与 tracking mode；
- `GenerationService` 显式接收 provider/session/index/gate 四项依赖；
- Controller 的 Responses store/get/delete 改走注入 index；
- `chatSession` 的 response rebind 改走注入 index；
- RequestAdapters 的 mode 由 runtime 同步配置，不再在解析期间定位 session singleton。

该工作包完成时，`chatSession` 已由 runtime 注入到 application/transport；其残余的
Meyers singleton 仅是测试兼容壳。P5-W3 随后删除该壳，并把 SessionJanitor 的
deadline 超支路径改为可用独立 store/persistence fake 验证，见
[`P05-controller-services-progress.md`](./P05-controller-services-progress.md)。

## 验证

- Debug reconfigure/build 通过；
- `ctest` 338/338 通过；
- architecture audit、cycle/layer、strict registration、source ownership、include、target DAG、
  queue/AppContext/deadline、ProviderRegistry 门禁通过；
- 新增 `check_session_services.py`：生产代码中 `ResponseIndex::instance`、
  `SessionExecutionGate::getInstance` 必须为 0，且 application/transport 中
  `chatSession::getInstance` 必须为 0；P5-W3 的 lifecycle gate 进一步要求全 production
  tree 中不存在 `chatSession::getInstance`。

## 回滚

必须整体回滚端口、AppContext ownership、Controller/Generation/Continuity 构造参数；禁止仅恢复
任一静态访问器，否则会形成两个 index/gate 实例并导致 response 连续性或并发门控分裂。
