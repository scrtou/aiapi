# P6-W3 Retool Provider 垂直切片（完成记录）

## 输入与范围

- P6-W1 已提供 `Result/Error/Deadline/CancellationToken`、JSON-free Provider DTO、`IChatProvider` 与
  final-NVI `ProviderBase`；
- P6-W2 已让 Chayns 走窄 Provider lane，但 Retool 仍是唯一使用
  `APIinterface::generate(session_st&)`、legacy registry fallback 的生产 Provider；
- 本项切换 Retool 的真实 workflow/agent 请求路径，同时删除这条 fallback 和宽端口本身；不提前重写
  P7 才处理的 GenerationService legacy event/session JSON materialization。

## 设计与实现

```text
AppWiring
  -> makeProductionProvider<retoolapi>(transport, clock, managedAccounts, workspaceUseCase, channels)
  -> retoolapi::initialize() -> Result<void>
  -> ProviderRegistry::registerChatProvider(chat, model catalog, thread context)
  -> registry freeze / publish

GenerationService
  -> session_st -> ProviderRequest（仅复制 workspace_id/workspaceId 到 routingHints）
  -> ProviderCallContext(read-only cancellation, absolute deadline)
  -> IChatProvider::generate
  -> ProviderBase::generate(final) -> retoolapi::doGenerate
  -> workflow | agent -> Result<ProviderResponse> / platform::Error
  -> application materializes the remaining legacy JSON/SSE event pipeline
```

- `retoolapi` 直接继承 `provider::ProviderBase`，并实现 `IProviderModelCatalog` 与
  `IProviderThreadContext`。启动改为 `initialize() -> Result<void>`，生成改为
  `doGenerate(ProviderRequest, ProviderCallContext) -> Result<ProviderResponse>`。
- workflow/agent 协议分支、模板 patch、workspace usage、agent thread 与 workspace affinity 都保留在
  Retool adapter 内。显式 workspace 只从 string-only `ProviderRequest::routingHints` 中读取；没有
  `session_st`、clientInfo JSON bag、HTTP headers 或 session response 越过 Provider 边界。
- `sendWithinContext()` 将每次 Retool HTTP timeout 剪裁为 context 剩余 deadline；workflow/agent 的
  polling、replay polling 与 sleep 在下一阻塞边界前后检查 cancellation/deadline。取消发生在 poll
  返回后时，不会再发下一次 poll。
- Retool 成功以 `ProviderResponse` 返回 text/metadata；失败以 `platform::Error` 返回。`ProviderBase`
  统一处理前置取消/过期、异常转换和一次性 failure observer，不重复上报。
- `eraseThreadContext()` 和 `transferThreadContext()` 显式管理本地 affinity/thread map；Retool 未提供
  稳定的远端 delete contract 时，`deleteUpstreamThread()` 返回 `NotFound`，不再静默 no-op。
- `ProviderRegistry` 删除 legacy map、`registerProvider()` 和 `findProvider()`；
  `IProviderRegistry::findChatProvider()` 成为必需窄能力。AppWiring 同时以 narrow chat/model/thread
  registration 发布 Chayns 与 Retool。`APIinterface.h`、残留 AccountManager declaration 和 obsolete
  forward declaration 一并删除。
- `GenerationService` 不再回退调用宽接口，也删除无业务语义的 legacy `afterResponseProcess()` 调用。
  `AiApiUseCase`、Session 与 Health readiness 同步删除 fallback。仍存在的 `ProviderResult` codec 是 P7
  前 application event JSON compatibility，不再是任何 Provider 的入口或出口。

## 契约、测试与门禁

新增或更新的离线覆盖：

- Retool workflow/agent fixture 的真实 HTTP wire 交互、metadata、workspace usage 与 affinity；
- workflow/agent HTTP 错误、非法 JSON、poll timeout、agent FAILED 与 cancellation poll boundary；
- Retool 的 narrow model/thread capability，以及明确的 upstream-delete unsupported result；
- GenerationService bridge fake、AiApiUseCase catalog、Health readiness、ProviderRegistry 和 Session
  thread cleanup/transfer 全部迁到窄 port；
- `check_retool_provider_slice.py` 固定 Retool inheritance、无 legacy session/result/singleton、workflow/
  agent/cancellation、runtime wiring 和 registry deletion。`--selftest` 在内存中破坏
  `ProviderBase` 继承并要求 gate 以 rc=4 拒绝；不写工作树。

同时更新 `check_chayns_provider_slice.py` 到 P6 完成态：两家留在 apipoint 目录期间，只有
`chaynsapi.h` 与 `retoolapi.h` 可包含 infrastructure `ProviderBase`；Chayns 不得因为 Retool 切片而
重新获得 legacy fallback。`check_provider_registry.py` 也冻结 `APIinterface` 与 legacy registry lane 的
删除，CI 注册两道 Retool gate（正常 + selftest）。

最终验证命令及结果：

```text
cmake -S . -B build                              PASS
cmake --build build -j1                          PASS
ctest --test-dir build --output-on-failure       PASS
build/src/test/aiapi_test                        PASS
python3 tools/arch/check_test_registration.py --require-strict PASS
python3 tools/arch/check_chayns_provider_slice.py --selftest   PASS
python3 tools/arch/check_retool_provider_slice.py --selftest   PASS
```

其余 architecture audit、cycle/layer、source ownership/include/target、enqueue、AppContext/deadline、
全部 P5 门禁和 P6 foundation/registry gate 亦在本提交前重跑。

## 遗留与下一步

- P6 的两家活跃 Provider 都已完成窄 `ProviderBase/Result` 切片，旧 `APIinterface` 和 registry dual lane
  已归零；下一工作项为 P7-W1，拆解 GenerationService 的 session/event JSON materialization 与 R1
  重复实现。
- Retool 的 workflow/agent template 与 HTTP 协议仍是 adapter 私有细节。P7 不应为了“统一”而把它们塞进
  ProviderBase；只有已经证实的可组合 policy 才能抽取。
- Retool upstream 没有稳定 remote-thread delete API，故只报告显式 unsupported/not-found；不得以空实现
  假装回收成功。

## 回滚

代码回滚必须成组撤回 Retool ProviderBase/Result 实现、application/registry/runtime 接线、删除的
`APIinterface`、fixture、P6-W3 gate、CI、layer-rule 说明及相关 ADR/计划/README。不得只恢复
`registerProvider()` 或对 Retool 增加 session-mutating adapter，否则会重新形成 narrow/legacy 双轨。
本项没有数据 schema、workspace 配置或上游协议状态迁移；回滚不需要数据恢复，但要恢复同版本的
Provider registry 与 runtime wiring。
