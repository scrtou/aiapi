# P7-W1 Generation pipeline 重写（完成记录）

## 输入与范围

- P1-W4 已用离线 fake Provider 锁定真实 `runGuarded` 路径、bridge/native tool、forced fallback、
  参数规范化和事件顺序；
- P5 已将 session/index/gate/channel/provider registry 改为注入协作者，P6 已将两家活跃 Provider
  收敛为 `IChatProvider::generate(ProviderRequest, ProviderCallContext) -> Result<ProviderResponse>`；
- 原 `GenerationServiceEmitAndToolBridge.cpp` 为 2,225 行，和原 `GenerationService` 一起承载请求
  物化、连续性、门控、Provider 调用、Codex retry、工具规则、事件发射和提交，无法按职责维护；
- 本项不改变公开 `GenerationService::runGuarded`、HTTP/SSE sink 或 Provider port，也不提前重写
  P7-W2 的 Account workflow。

## 设计与实现

`GenerationService` 现在是稳定的 application facade，只持有
`std::unique_ptr<generation::GenerationPipeline>`，构造后直接将 `runGuarded` 委派给 pipeline。旧大文件
已删除，不保留旧成员函数或 permanent forwarding lane。

```text
GenerationService::runGuarded
  -> GenerationPipeline::run
     -> materializeRequest
     -> ContinuityResolver + chatSession::getOrCreateSession
     -> ExecutionGuard / cancellation token / absolute deadline
     -> prepareToolBridge
     -> IChatProvider::generate(ProviderRequest, ProviderCallContext)
     -> optional Codex bridge retry
     -> GenerationResponsePipeline::emit
     -> coverSessionresponse + ResponseIndex bind
```

- `GenerationPipeline` 独占请求物化、连续性恢复、门控、请求级 bridge 状态、Provider DTO 边界、
  semantic `platform::Error`、Codex retry 与会话提交。Provider failure 保持原 `ErrorCode`、安全
  message、providerCode 和 diagnostics，不伪造为通用 502。
- `GenerationResponsePipeline` 独占输出清洗、native/bridge decode、forced tool、tool identity、参数
  规范化、schema 校验、严格客户端规则、zero-width session ID 和 event sequence。正常完成顺序固定为
  `ToolCallDone → OutputTextDone → Completed`；Provider 失败为 `Started → Error → close`。
- 原大文件中的纯 tooling 实现迁至 `ForcedToolCallGenerator.cpp`、`ToolCallNormalizer.cpp`、
  `ToolDefinitionEncoder.cpp`；headers 继续是窄函数端口，生产与测试使用同一实现。
- 为避免“拆文件却使 legacy owner 数上升”，将 Generation/session/tooling/continuity/action-protocol
  的可编译 support closure 一并迁至 `aiapi_application`，并从 `aiapi_legacy` 删除。该 target 只新增
  Drogon 的**私有外部** link requirement（遗留 `session_st`/JSON event compatibility 所需），没有新增
  internal target reverse edge；这个 compatibility debt 留给 P8。`AIAPI_LEGACY_SOURCES` 从 38 降至 20。

## 行为与门禁

新增 `test_generation_service_bridge_fixture.cpp` 的
`GenerationService_ProviderFailurePreservesSemanticErrorAndCloses`：内存 Provider 返回 `RateLimited`，
断言 Started、Error、close 的顺序，以及 `ErrorCode`、providerCode、detail 不丢失。既有 production-path
fixture 继续覆盖：请求 bridge 注入、bridge decode/event order、native empty arguments 规范化和
`tool_choice=required` fallback。

新增 `tools/arch/check_generation_pipeline_slice.py` 并接入 `arch-cycles.yml`。它冻结：

1. 旧 `GenerationServiceEmitAndToolBridge.cpp` 已删除；
2. facade 只保存 `GenerationPipeline`，不恢复物化/Provider/emit 成员；
3. 三个 core 文件和三个 tooling 文件由 `aiapi_application` 唯一拥有、不得回到 legacy；
4. request 和 response stage 的关键入口及顺序存在；
5. 生产 fixture 覆盖上述边界。

`--selftest` 仅在内存中把 facade delegation 改成空返回，并确认 gate 以 rc=4 拒绝，不改写工作树。
P6 Chayns/Retool gates 同步改为检查 `GenerationPipeline::invokeProvider`，P5 session-application gate
同步覆盖新的 stage 文件。

## 验证结果

```text
cmake -S . -B build                              PASS
cmake --build build -j1                          PASS
ctest --test-dir build --output-on-failure       PASS (393/393, 26.98s)
build/src/test/aiapi_test                        PASS (393 cases / 2053 assertions)
python3 tools/arch/check_test_registration.py --require-strict
                                                   PASS (393 declared / 393 registered)
python3 tools/architecture_audit.py --selftest   PASS
python3 tools/architecture_audit.py --baseline doc/adr/audits/audit-baseline.json
                                                   PASS
python3 tools/arch/check_generation_pipeline_slice.py [--selftest]
                                                   PASS
```

完整 cycle/layer/db/source-owner/include/target/enqueue/AppContext/deadline、P5 injection、P6 foundation/
Chayns/Retool（含 selftest）门禁也在提交前重跑。架构审计为 `R1=0`、`R3=7 / 2889 lines`；相对干净
基线的 `R1=4`、`R3=13 / 5006 lines` 均下降，Generation 相关超长函数已退出 R3 列表。

## 遗留与下一步

- `session_st`、JsonCpp/Drogon event compatibility 仍在 application boundary；本项只重写 pipeline
  ownership，P8 再清理过渡表示和 legacy target；
- 下一工作项是 **P7-W2 Account workflows 重写**：先提取 selector/rotation、state transition/rollback，
  再迁移 token refresh、registration 和 health workflow；
- Provider 的 wire protocol、session persistence 和 sink serialization 未改变，不应为“进一步统一”而
  移入本 pipeline。

## 回滚

这是纯代码/构建图变更，无数据 schema 或上游配置迁移。回滚必须作为一个整体撤回：facade/pipeline/
response/tooling source owner、CMake application closure、P7 gate/CI、P6/P5 gate 定位更新、fixture 和文档。
不得只恢复旧大文件或只把某个 stage 放回 legacy，否则会同时恢复双实现并破坏 source-owner ratchet。
