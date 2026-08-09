# 重构工作簿与阶段产物

每个工作包必须先更新本文状态，再修改代码；完成后补充测试、依赖图和回滚说明。状态：`TODO`、
`DOING`、`DONE`、`BLOCKED`。

| ID | 工作包 | 主要文件/目录 | 必交产物 | 状态 |
|---|---|---|---|---|
| W0 | 基线与行为快照 | `tools/`、`src/test/` | clean baseline、覆盖报告、假上游 fixture | DOING |
| W1 | 生产 library 化 | `src/CMakeLists.txt`、`src/test/CMakeLists.txt` | [`W01-production-targets.md`](./work-products/W01-production-targets.md)；target DAG、无 `PROJECT_SOURCES` | TODO |
| W2 | Result/Deadline/Cancel | `src/platform/` | Result contract、取消测试、nodiscard 门禁 | TODO |
| W3 | domain codec 净化 | `src/domain/`、`src/infrastructure/codec/` | 模型 codec 往返测试、domain 第三方依赖为 0 | TODO |
| W4 | BoundedExecutor | `src/utils/BackgroundTaskQueue.h` → `infrastructure/executor` | 四态队列、容量、drain/shutdown 集成测试 | TODO |
| W5 | AppContext/runtime | `src/main.cc` | Builder、RouteRegistrar、ShutdownCoordinator | TODO |
| W6 | ProviderRegistry | `src/apiManager/`、`src/infrastructure/provider/` | ProviderBase NVI、编译期继承检查 | TODO |
| W7 | Session vertical slice | `src/sessionManager/core/`、`continuity/` | SessionStore/Continuity/Index port 和迁移测试 | TODO |
| W8 | Chayns Provider | `src/apipoint/chaynsapi/` | Chayns contract suite、Provider 无 session 副作用 | TODO |
| W9 | Retool Provider | `src/apipoint/retoolapi/` | workflow/agent fixture、workspace usage 一致性测试 | TODO |
| W10 | Account workflows | `src/accountManager/`、`managedAccount/` | selector/state machine/workers、失败回滚测试 | TODO |
| W11 | Controllers/sinks | `src/controllers/` | Controller 只依赖 use case、SSE loop-affinity 测试 | TODO |
| W12 | Provider retirement | `nexos/openai`、`tools/migrations/` | dry-run/archive/restore/410 验证报告 | TODO |
| W13 | 收口 | 全部 `src/` | R1/R3、ASan/TSan、架构基线、文档发布 | TODO |

## 产物保存规则

- 每个 W 包在 `doc/adr/work-products/Wxx-*.md` 保存设计、调用图、测试结果和回滚方式；
- 机器数字只从 `architecture_audit.py` 生成，不手工复制；
- 代码和文档可以分提交，但工作包状态只能在对应门禁通过后改为 `DONE`；
- 发现调用图或所有权假设不成立时，先将工作包标为 `BLOCKED` 并更新 ADR，不在旧假设上继续编码。
