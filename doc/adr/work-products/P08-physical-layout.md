# P8-W2 物理目录收口

> 状态：**DONE**（2026-08-15）
> 前置：[`P08-transition-cleanup.md`](P08-transition-cleanup.md) 的 P8-W1 target/DAG 收口已完成。
> 范围：将仍留在历史顶层目录的源码真正迁入六个正式层，并让该事实受 CI 约束；不改变 HTTP 协议、
> 数据库 schema、Provider 上游协议或既有 Provider archive。

## 输入与退出目标

P8-W1 已让每个 production implementation 由正式 CMake target 唯一拥有，但目标 owner 与物理路径
仍是两个不同事实：`sessionManager/`、`dbManager/`、`controllers/` 等历史顶层目录仍存在。因此仅通过
`check_target_layers.py` 不能阻止新代码在错误目录落盘。

本工作项的退出条件：

1. `src/` 顶层只保留 `application`、`domain`、`infrastructure`、`platform`、`runtime`、`test`、`transport`
   及根 `CMakeLists.txt`/`main.cc`；
2. 每个 `AIAPI_*_SOURCES` implementation 位于所属 target 的物理层目录；
3. include、测试、架构 gate、CI mutation probe 和文档全部使用 canonical path；
4. P8-W2 实现提交后，从干净工作树重新生成并单独提交 architecture audit baseline。

## 设计与迁移

### 目录归属

```text
application/{account,channel,generation,health,metrics,workspace}
infrastructure/{account,codec,config,executor,managedAccount,metrics,persistence,provider,workspace}
transport/controllers/{codecs,sinks}
```

- 账号、渠道、generation 和 workspace application facade 进入 `application/`；账号 concrete clock/HTTP
  adapter 进入 `infrastructure/account/`，`IAccountClock`/`IAccountHttpTransport` 留在 `domain/port/`。
- DB、metrics、managed-account、Retool workspace service/codec 和 provider budget 进入
  `infrastructure/`；`ManagedAccountService` 的 CMake owner 同步改为 infrastructure。
- Controller、filter、SSE/JSON sink 和 HTTP codec 进入 `transport/controllers/`。Retool HTTP JSON codec
  为避免 transport 反向依赖 infrastructure 维持 transport 专用副本；infrastructure 的 DB/adapter codec
  仍在 `infrastructure/codec/`。
- 已无调用方的 `apiManager/Apicomn.h` 删除；Drogon model 描述迁至 `tools/drogon/model.json`。

### 防回归

新增 `tools/arch/check_physical_layout.py`：

- 列出唯一允许的 `src/` 顶层目录，并明确拒绝所有历史目录；
- 解析 `src/CMakeLists.txt` 的正式 source list，检查每个 implementation 的路径前缀与存在性；
- `--selftest` 在内存中同时构造错误层 source 与复活的 `sessionManager/` 顶层目录，二者都必须以
  rc=4 被拒绝。

物理收口还暴露了旧 `--db-ratchet` 的一个前提：它原来按 `dbManager` 顶层模块识别 DB header，
目录迁移后会把 `infrastructure/persistence/` 误认为与所有 infrastructure 文件相同的模块，从而静默漏报。
保留 CLI/JSON 名称以兼容现有 CI，但 gate 现在按实际 persistence header 位置解析，冻结 provider/runtime
直连，并明确要求 `application/`、`transport/` 保持零直连；其 mutation probe 仍必须返回 rc=4。基名唯一性
检查也提升为全库路径级，避免同一正式层不同子目录的同名 header 被错误解析。

CI 在 source ownership 与 target-DAG gate 后运行正向 gate 与 selftest。目录 gate 只守“路径与 target
一致”；owner 数量和真实编译证据仍由 source-ownership gate 负责。

## 验证

本次收口的实测结果：

| 项 | 结果 |
|---|---|
| physical-layout 正向 gate 与 selftest | **PASS**；正式 source list 均位于匹配层，selftest 同时证明错误层 source 和复活的 `sessionManager/` 根目录会被拒绝 |
| clean Debug configure/build + CTest | **PASS**；396/396 tests |
| 全部 static architecture/迁移 gate 与 selftest | **PASS**；audit、cycle/layer/persistence ratchet、strict registration、owner/include/target、ADR-09、P4～P7 及 P8-W2 gate 全部通过 |
| coverage | **PASS**；396/396 tests，89 个 production implementation 中 77 个进入 instrumented 对象图，行 7303/14614（49.97%），分支 9821/39050（25.15%） |
| clean ASan+UBSan | **PASS**；396/396 tests；`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` 与 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` 下无 sanitizer/runtime diagnostics |
| clean TSan | **PASS**；396 cases / 2075 assertions，46 条预期环境 warning、0 data race；4 个 shutdown 专项各 5/5，idle/http/polling/backlog/disconnect SIGTERM harness 各 5/5 |
| clean implementation commit 后的 audit baseline | 作为与实现分离的 machine snapshot 提交，保证生成时 `git.dirty=false` |

## 退出结论

P8-W2 已使 production source 的 CMake owner、物理路径和六层 target DAG 成为同一份事实：历史顶层
源码目录已删除，根目录只保留正式层、`CMakeLists.txt` 与 `main.cc`，并由 physical-layout gate 持续
防回归。实现提交与 audit baseline 刻意分离；后者只在实现提交后的干净工作树生成，避免把脏树快照
误当作发布基线。

## 回滚

如必须回滚，应整体回滚本工作项的目录移动、include/CMake 路径、physical-layout gate、CI 及文档，
不能只恢复某一个历史目录或为旧路径增加 include root。部分回滚会同时破坏 source ownership、layer
规则和 target DAG 的可解释性。
