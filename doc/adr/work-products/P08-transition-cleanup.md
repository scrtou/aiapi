# P8-W1 过渡代码和 target debt 清理（完成记录）

> 收口日期：2026-08-15。本文只记录 P8-W1 的代码/构建 target 边界收口，不改变数据库 schema、
> Provider 上游协议或已归档 Provider 数据。它曾错误地把 target owner 收口表述为整个 P8 已完成；
> 历史顶层目录的实际迁移由后续 [`P08-physical-layout.md`](P08-physical-layout.md) 的 P8-W2 负责。

## 输入与退出目标

P7-W2 后，`aiapi_legacy` 仍有 19 个 source owner，application 的请求/事件边缘仍携带
Drogon compatibility，部分 Provider/queue/config 文件还留在历史目录。本项的退出条件是：

1. 删除 `aiapi_legacy` target 和 `AIAPI_LEGACY_SOURCES`，所有 production implementation 只属于六个
   正式 target；
2. 清理已无调用方的 ProviderResult/重复错误表示和过渡 allowlist；
3. 以中立 DTO 隔离 HTTP/runtime IO，application 不再接收或链接 Drogon；
4. P8-W1 相关 CI gate、README、Provider archive 运维入口与架构文档同步为 target/DAG 完成态；
   物理目录是否收口不属于本工作项；
5. clean Debug/coverage/ASan+UBSan/TSan、全量测试、SIGTERM harness 和架构门禁均通过，并从干净实现提交
   生成新的 audit baseline。

## 实现与调用边界

### 正式 target DAG

所有 production `.cpp/.cc` 现在由一个正式 target 唯一拥有（89 个实现）；测试链接这些 target，
不再维护生产源码副本。最终依赖关系为：

```text
aiapi_application ──> aiapi_domain ──> aiapi_platform
aiapi_infrastructure ──> aiapi_domain, aiapi_platform, Drogon/OpenSSL/PostgreSQL
aiapi_transport ──> aiapi_application, aiapi_domain, aiapi_platform, Drogon
aiapi_runtime ──> aiapi_application, aiapi_infrastructure, aiapi_transport
aiapi (main.cc) ──> aiapi_runtime (+ whole-archive aiapi_transport)
```

`aiapi_transport` 的 whole-archive 只保留 Drogon 静态 Controller 注册对象；它不是 application 的
反向依赖。`check_source_ownership.py --require-no-legacy` 和
`check_target_layers.py --require-no-legacy` 同时拒绝旧 target/source list 复活。

最后的闭包按职责落到正式 target：Provider/reaper、DB/metrics/config adapter 和 history/outbound budget
进入 `aiapi_infrastructure`；channel、managed-account、workspace、AI use case/request adapter 进入
`aiapi_application`；所有 Controller/tombstone/log 进入 `aiapi_transport`。这说明 CMake owner 与
依赖边界正确，**不说明每个实现已经离开历史顶层目录**；该物理事实由 P8-W2 单独收口。

### ADR-09 HTTP / IO seam

```text
Drogon HttpRequest
  -> AiApiController: copy Json::Value + RequestHeaders
  -> AiApiUseCase / RequestAdapters (application values only)
  -> Generation pipeline / domain ports

Account workflow
  -> IAccountHttpTransport(HttpRequest/HttpResponse DTO)
  -> infrastructure/account/DrogonAccountHttpTransport

AppWiring: custom_config value
  -> AccountManager::setRuntimeConfig
  -> AiApiUseCase constructor
```

- `RequestAdapters` 删除 `drogon::HttpRequestPtr` 重载；Controller 承担 header 复制和 JSON 解析。
- `IAccountHttpTransport` 只暴露方法、路径、header、body 和结果 DTO；唯一 Drogon 实现在
  `infrastructure/account/`，由 `AppWiring` 在 Account 初始化前注入。
- application 将 UUID、日志、zero-width 编码改用 platform utility，CMake 只私有链接 JsonCpp 值 codec，
  不再链接或 include Drogon。
- `transport/controllers/sinks/IoLoopResponseStream` 保持为 worker 回到 Drogon event-loop 的 transport adapter。
  `check_http_io_boundary.py` 同时检查 include/link、DTO adapter 和 runtime-config 接线；其内存 mutation
  selftest 会验证注入 Drogon include 后 gate 必须失败。

### 过渡表示与运维清理（物理目录迁移另见 P8-W2）

- 删除 `domain/model/ProviderResult.h`、`application/generation/core/ProviderResultCodec.h`、
  `application/generation/core/Errors.h` 及对应测试；Provider 失败统一用
  `platform::Result<ProviderResponse, platform::Error>` / `platform::ErrorCode` 表示。
- Chayns/Retool 实现迁至 `infrastructure/provider/{chayns,retool}`；queue、ConfigValidator、
  zero-width encoder、response stream、browser helper 和 login summary 均迁至各自层目录。
- retired-provider gate 跟随 Account workflow 拆分扫描 `AccountSelector.cpp` 与
  `AccountRegistrationWorkflow.cpp`，防止物理搬迁漏掉 nexos/openai 写入拦截。
- 删除已过时的 `tools/tsan_probe/` 窄探针和历史结果；发布级并发证据统一使用
  `tools/run-tsan.sh`（全量测试、停机专项和五类 SIGTERM 场景）。
- Provider 数据 archive/restore 没有随代码下线删除：根 README 指向
  `tools/migrations/README.md` 与 P02 work-product，仍要求维护窗口内按备份、preflight、演练、retire 的顺序执行。

## 测试与门禁

### Debug 与 coverage

| 项 | 结果 |
|---|---|
| Debug configure/build | PASS |
| Debug `ctest --test-dir build --output-on-failure` | **396/396 PASS** |
| 直接 `build/src/test/aiapi_test` | **396 cases / 2075 assertions PASS** |
| coverage configure/build + CTest | **396/396 PASS** |
| coverage 机器快照 | [`P08-coverage.md`](P08-coverage.md) / [`P08-coverage.json`](P08-coverage.json) |

覆盖快照仅由生成器维护数字：89 个 production implementation 中 77 个进入 instrumented 测试对象图，
行覆盖为 7306/14614（49.99%），分支覆盖为 9826/39050（25.16%）。高风险 generation、Provider、
Account workflow、queue、streaming 和 shutdown 函数均有实际执行证据；未进入对象图的 adapter/启动源
在机器报告中显式列为 `not_instrumented`，不伪造分母。

### Sanitizer、停机与结构棘轮

| 项 | 结果 |
|---|---|
| clean ASan+UBSan configure/build + CTest | **396/396 PASS**；`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` 与 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` 下无 sanitizer/runtime diagnostics |
| `tools/run-tsan.sh`（全量、停机专项 x5、五类 SIGTERM x5） | **全绿：396 cases / 2075 assertions PASS**；46 条预期环境 warning、**0 data race**；4 个 shutdown 专项各 **5/5**，idle/http/polling/backlog/disconnect 五类 SIGTERM harness 各 **5/5** |
| CI `arch-cycles.yml` 的正向 gate 与所有 selftest | **30/30 PASS（P8-W1 时）**：audit、cycle/layer/db ratchet、startup、strict registration、owner/include/target、retired-provider、ADR-09、queue/AppContext/deadline、P5/P6/P7 gates 与 provider-retirement fixture 均通过；P8-W2 另加入 physical-layout gate |
| architecture audit | 现有 baseline ratchet **PASS**：R1=0，R2=41，R3=6/2563；89 个 production implementation、65 个测试源、396 cases。P8 实现提交后从干净工作树生成并单独提交新的 machine baseline（`git.dirty=false`） |

## 退出结论、遗留与回滚

P8-W1 不再保留 `aiapi_legacy`、过渡 owner、ProviderResult compatibility 或 application-to-Drogon
依赖。JsonCpp 仅作为 application request/event 的值表示，既不泄入 domain，也不携带 HTTP/runtime 类型；
这不是 legacy target debt。P8-W1 不应被用来声称历史物理目录已删除；该退出结论属于 P8-W2。

本项没有数据迁移。若必须回滚 P8-W1，应整体回滚其 target/CMake owner、adapter、gate、测试和文档，
不能只恢复旧 compatibility shim，否则会重新形成双轨并破坏 source-owner ratchet。若回滚 P8-W2，则还必须
整体回滚目录移动、include 路径与 physical-layout gate，不能只恢复某个历史目录。Provider 数据的恢复仍严格按
`tools/migrations/README.md` 与 P02 archive manifest 执行，不能用代码回滚替代。
