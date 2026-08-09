# P3-W1 · production target 唯一 source owner

| 项 | 值 |
|---|---|
| 状态 | DONE |
| 前置 | P0 clean baseline；P1 行为安全网；P2 Provider 退役 |
| 目标 | 消除测试侧生产源复制，使每个生产 `.cpp/.cc` 只有一个 production owner |
| 决策 | [ADR-11](../decisions/ADR-11-production-test-targets.md) |

## 1. 输入与边界

### 1.1 输入

- 变更前 `src/CMakeLists.txt` 把 `main.cc` 和所有生产实现直接编入 `aiapi`；
- 变更前 `src/test/CMakeLists.txt` 另维护 54 个 `PROJECT_SOURCES`，重复编译生产实现；
- `source-audit-2026-08.md` 登记了目标分层，但尚未形成可由 CMake 验证的 target 边界；
- 现有测试依靠 `stub_db_collaborators.cpp` 隔离 DB 协作者，不得因链接模型变更而访问真实数据库或上游。

### 1.2 本工作项做什么

1. 建立临时静态库 `aiapi_legacy`，唯一持有除 `main.cc` 外的生产实现；
2. `aiapi` 和测试 executable 链接同一份生产 object；
3. 删除 `PROJECT_SOURCES`、测试直接编译 `../*.cpp` 和 shutdown fixture 的生产源复制；
4. 在 configure、静态 CI 和 compile database 三个层面校验 source ownership；
5. 使 coverage 改为采集测试进程实际执行的生产库 object，不再依赖测试重复编译。

### 1.3 本工作项不做什么

- `aiapi_legacy` **不是最终架构**，不表示 platform/domain/application/infrastructure/transport 已分层；
- 本步不改业务流程、Provider 契约、DB schema 或 HTTP 协议；
- 本步不将测试改成 whole-archive，也不试图在 P3-W1 一次建立全部正式分层库。

## 2. 变更前后构建图

### 2.1 变更前

```text
aiapi
  └─ main.cc + 67 个其他生产实现（直接编译）

aiapi_test
  ├─ 41 个 TEST_SOURCES
  ├─ 54 个 PROJECT_SOURCES（再次编译生产源）
  └─ stub_db_collaborators.cpp

aiapi_shutdown_signal_fixture
  ├─ support/shutdown_signal_fixture.cpp
  └─ utils/ApplicationShutdown.cpp（再次编译）
```

这个图存在两个真值源：production 清单和 test 清单可以独立漂移；
测试通过不等价的编译闭包，不能直接证明主程序使用的是同一 object。

### 2.2 P3-W1 之后

```text
aiapi_legacy STATIC（67 个生产实现）
  │
  ├─> aiapi（main.cc）
  │      └─ --whole-archive aiapi_legacy --no-whole-archive
  │
  ├─> aiapi_test（41 个 TEST_SOURCES，包含协作者 stub）
  │      └─ 普通静态库链接
  │
  └─> aiapi_shutdown_signal_fixture
         └─ 普通静态库链接
```

`src/` 共有 68 个生产 `.cpp/.cc`：`main.cc` 的 owner 是 `aiapi`，
其余 67 个的 owner 是临时 `aiapi_legacy`。

## 3. source ownership 契约

### 3.1 CMake configure 门禁

`aiapi_register_production_sources(owner ...)` 对路径取绝对值，并用 global
property 记录 owner。同一源再次登记时 configure 立即失败，不等到链接期。

生产 source list 统一使用 `AIAPI_*_SOURCES` 命名。这个命名同时是
`architecture_audit.py` 和 `check_source_ownership.py` 的机器可读契约；后续
carve out 新 target 时必须继续遵守，或者同步升级解析器。

### 3.2 静态 CI 门禁

`tools/arch/check_source_ownership.py` 在没有构建产物时也会：

- 扫描 `src/` 下除 `src/test/` 外的所有 `.cpp/.cc`；
- 要求 `main.cc + AIAPI_*_SOURCES` 精确覆盖生产集，owner 数必须为 1；
- 拒绝测试 `PROJECT_SOURCES`、`../*.cpp/.cc` 和缺失生产库链接；
- 在 CI 的 `arch-cycles` workflow 中持续执行。

CI 门禁自检会临时增加一个无 owner 的生产 `.cpp`，并断言检查器
确实以 rc=1 失败，防止门禁被误改成“永远绿色”。

### 3.3 本地严格门禁

configure 输出 `build/compile_commands.json`，严格门禁额外对每个生产源
统计真实 compile command，要求 68 个文件每个恰好出现一次：

```bash
python3 tools/arch/check_source_ownership.py \
  --compile-commands build/compile_commands.json
```

## 4. 链接语义与测试隔离

### 4.1 为什么主程序使用 whole-archive

`chaynsapi.cpp` 和 `retoolapi.cpp` 中的 `IMPLEMENT_RUNTIME` 通过静态对象向
`ApiFactory` 注册 Provider。`main.cc` 没有对这些注册 object 的普通未解析符号引用。
若按普通静态库规则链接，linker 可以不提取该 object，于是构建成功但
runtime factory key 消失。GNU/Clang 下的 production executable 因此使用：

```text
-Wl,--whole-archive libaiapi_legacy.a -Wl,--no-whole-archive
```

这是对现有静态注册模式的过渡性保真，不是鼓励新代码继续增加隐式注册。

### 4.2 为什么测试不使用 whole-archive

`aiapi_test` 依赖 `stub_db_collaborators.cpp` 为 ConfigDbManager、RetoolWorkspaceService 和
ChaynsThreadDbManager 的未解析协作者符号提供离线实现。普通链接只从
`aiapi_legacy` 提取仍需的 object；已被 stub 满足的 DB object 不会被无条件拉入。

如果测试也使用 whole-archive，真实 DB 实现和 stub 会同时进入链接，
可能产生重复定义，并破坏“测试不访问真实数据库”边界。
`channelDbManager.cpp` 是例外：AccountManager 需要其 vtable，所以 linker 从生产库
提取该 object；用例不调用其 DB 方法。

### 4.3 符号证据

production link command 包含 whole-archive，test/fixture link command 只包含普通 archive：

```text
aiapi: -Wl,--whole-archive libaiapi_legacy.a -Wl,--no-whole-archive
aiapi_test: ../libaiapi_legacy.a
aiapi_shutdown_signal_fixture: ../libaiapi_legacy.a
```

`nm -C` 同时在 archive 和测试 executable 中找到生产符号：

```text
libaiapi_legacy.a:
0000000000004164 T GenerationService::runGuarded(...)

aiapi_test:
0000000000729394 T GenerationService::runGuarded(...)
```

`nm` 只是链接符号证据；函数是否运行由 gcov 报告证明，不用 `nm`
冒充运行时覆盖。

## 5. 非生产工具分类

`src/tools/accountlogin/login_client.cpp` 是独立运维示例，依赖当前项目 CMake
没有声明的 curl 和 nlohmann/json，也从未属于 `aiapi` executable。将整个
`accountlogin` 目录移到 `tools/accountlogin/`，并用独立 README 明确“不构建、不运行、
需手工授权”的边界。它不再伪装成 `src/` 下的无 owner 生产实现。

历史审计文档中的旧路径是当时真值，不回写；重新生成的 coverage
报告已自然移除该项。

## 6. 构建入口与 coverage

- 测试必须从仓库根目录 configure；单独 `cmake -S src/test` 会明确失败，
  避免为了 standalone build 重建第二份生产源清单；
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON` 为 source-owner 严格门禁和后续 include 审计提供输入；
- architecture audit v4 把 `AIAPI_*_SOURCES` 称为“已登记 production owner”，
  不再错称为“已进测试链接”；静态源清单无法证明 archive member 被提取；
- coverage 编译选项作用于生产库和各 executable；shutdown fixture 也链接
  gcov runtime，否则它从 instrumented static library 提取 object 时会出现
  `__gcov_init/__gcov_exit` 未解析符号；
- `generate_report.py` 采集 `aiapi_legacy.dir` 和 `aiapi_test.dir` 的 gcda。前者证明
  测试实际运行的生产 object，后者补充在测试 TU 中实例化的生产 inline/header 代码。

P3-W1 重新生成的机器报告见
[`P01-runtime-coverage-report.md`](P01-runtime-coverage-report.md)：68 个 production 实现，
53 个进入测试链接对象图，高风险目标函数仍有运行时执行证据。

## 7. 验证记录

| 门禁 | 结果 |
|---|---|
| normal configure/build | PASS |
| normal `ctest` | 260/260 PASS |
| coverage configure/build | PASS |
| coverage `ctest` | 260/260 PASS |
| coverage 机器报告 | PASS；68 production，53 linked/instrumented |
| ASan configure/build + `ctest` | 260/260 PASS（`detect_leaks=0:halt_on_error=1`） |
| source ownership（静态 + compile database） | PASS；68/68 每个 owner/compile 恰好一次 |
| test registration strict | PASS；260 声明 = 260 注册 |
| architecture selftest/ratchet | PASS |
| cycle/layer/startup/retired-provider 门禁 | PASS |
| `git diff --check` | PASS |

覆盖率数字只在机器报告中维护；本文不复制行/分支百分比，避免形成
第二个会过期的基线。

## 8. 回滚

本步只改构建图和工具分类，不改 DB schema 和线上数据：

1. 恢复上一提交的 `src/CMakeLists.txt` 和 `src/test/CMakeLists.txt`；
2. 将 `tools/accountlogin/` 移回原路径；
3. 恢复 coverage collector 的旧 target marker；
4. 从空 build directory 重新 configure/build/test。

回滚不需要数据恢复。若只是后续 carve-out 失败，应把该闭包放回
`AIAPI_LEGACY_SOURCES`，而不恢复 `PROJECT_SOURCES` 双轨。

## 9. 后续 carve-out 顺序

P3-W1 只提供单 owner 脚手架。后续严格按当前 P3 工作项执行：

1. P3-W2：收敛为从 `src/` 起的完整 include，只保留单一 include 根；
2. P3-W3：按 platform → domain → application → infrastructure → transport → runtime
   每次 carve out 一个可编译闭包，立即从 legacy 删除；
3. P3-W4：按模型建立 edge codec，domain 移除 `Json::` 和第三方 IO 头；
4. 当正式 target 全部接管且 `AIAPI_LEGACY_SOURCES` 为空时，删除 `aiapi_legacy`。

每次移动都必须重跑 source ownership、clean build、全量测试、layer/cycle
和 link 证据；不得复制源文件来制造两个 owner。
