# P6-W2 Chayns Provider 垂直切片（完成记录）

## 输入与范围

- P6-W1 已建立 `Result/Error/Deadline/CancellationToken`、JSON-free Provider DTO、
  `IChatProvider` 与 final-NVI `ProviderBase`，但没有伪装迁移任何生产 Provider；
- 本项只切 Chayns 的真实调用链，Retool 的 `APIinterface/session_st&` fallback 明确保留给 P6-W3；
- 保持现有 Chat/Responses JSON/SSE、Pro/free 路由、continuation、工具桥接和 Chayns thread ledger 行为。

## 设计与实现

```text
AppWiring
  -> makeProductionProvider<chaynsapi>(IAccountSelector, transport, clock, ledger)
  -> chaynsapi::initialize() -> Result<void>
  -> ProviderRegistry::registerChatProvider(chat, model catalog, thread context)

GenerationService
  -> SessionExecutionGate lease owns CancellationSource
  -> ProviderRequest + ProviderCallContext(read-only token, absolute deadline)
  -> IChatProvider::generate
  -> ProviderBase::generate(final) -> chaynsapi::doGenerate
  -> Result<ProviderResponse> / platform::Error
  -> application materializes only the remaining legacy event/session JSON
  -> JSON/SSE sink maps platform ErrorCode through the single HTTP mapping
```

- `chaynsapi` 不再继承 `APIinterface`，而是直接继承 `provider::ProviderBase`；入口变为
  `initialize() -> Result<void>` 和 `doGenerate(ProviderRequest, ProviderCallContext)`。
- 新增 `IProviderModelCatalog`、`IProviderThreadContext`；registry 分离 narrow/legacy lane，同一名称不能
  同时注册，因而已迁移 Chayns 无法静默退回 Retool 暂用的宽端口。
- Chayns 不再 include `Session.h` 或写读 `session_st/session.response`。它以
  `previousConversationId` 找回 provider-owned thread context，并以扁平 `chayns.*` metadata 返回锚点、
  reasoning、账号类型和 Pro workspace 信息。
- `GenerationService` 优先走 `findChatProvider()`，将 legacy session 显式转换为 `ProviderRequest`；成功 response
  在 application 边界回填剩余 event pipeline 所需 JSON，失败 `platform::Error` 不再被一律降级成
  `ProviderError`。Retool fallback 仍是唯一允许的 `findProvider()` 生成路径。
- 账号等待、HTTP 后、外层重试和 polling 都观察 cancellation/deadline；`sendWithinContext()` 将每次 HTTP timeout
  限为 context 剩余预算。`SessionExecutionGate` 改由 `CancellationSource` 持有取消权，旧 CancelPrevious guard
  不能在后续完成时清掉新 lease。
- `chatSession` 的 transfer/erase、`AiApiUseCase` 的 catalog 和 `chaynsThreadReaper` 的上游删除均优先使用
  窄 capability；Reaper 不再从 legacy lane 解析 Chayns。

## 契约、测试与门禁

新增/更新的离线覆盖：

- Chayns fixture 真实窄 port：free、Pro、follow-up、poll timeout；
- 在 create-thread HTTP 返回后触发 cancellation，确认不会进入下一次 polling；
- Chat JSON/SSE 与 Responses JSON/SSE 全部走 `registerChatProvider` 生产 sink 路径；
- 未知模型产生 `BadRequest`，确认 transport 返回 400 而不是统一 502；
- registry 的 narrow capability、名称跨 lane 排斥、session transfer/cleanup thread context；
- CancelPrevious token 传播及旧 lease 不得释放新 lease；
- reaper shutdown fixture 改走 `registerChatProvider`。

新增 `tools/arch/check_chayns_provider_slice.py` 并接入 CI。它冻结：

1. Chayns 生产目录不得重现 `session_st`、`APIinterface`、`Session.h`、`session.response` 或项目 singleton；
2. `ProviderBase`/`doGenerate`、model/thread capability、production factory 与 narrow registration；
3. GenerationService 优先 narrow lane、session/reaper capability 及 lease identity；
4. focused fixture/registry coverage 存在。

`--selftest` 在内存中破坏 `ProviderBase` 继承，并验证 gate 以 rc=4 拒绝该变异，不写工作树。

最终验证命令和结果：

```text
cmake -S . -B build                              PASS
cmake --build build -j1                          PASS
ctest --test-dir build --output-on-failure       PASS 390/390
build/src/test/aiapi_test                        PASS 390 cases / 2031 assertions
python3 tools/arch/check_chayns_provider_slice.py --selftest  PASS
python3 tools/arch/check_test_registration.py --require-strict PASS (390/390)
```

全量 architecture audit、cycle/layer/db、strict test registration、source ownership/include/target、
enqueue、AppContext/shutdown、全部 P5 门禁、P6 foundation gate 与 P6-W2 gate 均通过。

## 遗留与下一步

- Retool 仍是唯一 legacy Provider，P6-W3 必须按同一 Result/ProviderBase contract 切片；完成前不得删除
  `IProviderRegistry::findProvider()` fallback。
- P7 才会拆除剩余 legacy GenerationService event/session JSON materialization；本项不为此提前复制一个第二 pipeline。
- Chayns reaper 的已发出远端 DELETE 仍受 P4 shutdown budget 约束；其可中断 stop/ledger retry 语义不在本项改变。

## 回滚

代码回滚必须成组撤回 Chayns class/registry/application/session/reaper 接线、取消 lease 修复、fixture、
P6-W2 gate、CI/README/ADR/计划；不得只把 Chayns 放回 `registerProvider`，否则会形成 narrow/legacy 双轨。
无数据 schema、账号配置或上游线程格式变更；已写入的 thread ledger 与原有 schema 兼容，无数据迁移回滚步骤。
