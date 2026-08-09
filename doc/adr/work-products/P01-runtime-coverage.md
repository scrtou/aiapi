# P1-W1 · 运行时行/分支覆盖基线

| 项 | 值 |
|---|---|
| 状态 | DONE |
| 目标 | 证明被重构的生产实现真实执行，而不是仅被 include 或链接 |
| 前置 | P0 clean baseline 已完成 |

## 1. 范围

必须单独报告以下生产路径的行/分支覆盖：

- `chaynsapi::generate/postChatMessage`；
- `GenerationService::transformRequestForToolBridge/emitResultEvents`；
- AccountManager 账号选择、失效、回滚、池重建；
- `BackgroundTaskQueue` shutdown/drain；
- Chat/Responses 四种流式/非流式入口。

## 2. 实施步骤

1. 增加 CMake 选项 `AIAPI_ENABLE_COVERAGE`；
2. GCC 使用 `--coverage -O0 -g`，Clang 使用 `-fprofile-instr-generate -fcoverage-mapping`；
3. coverage flags 只挂在生产 library/测试 target，不污染 release；
4. 建立独立 `build-coverage/`，运行全量测试；
5. 生成机器报告和可读摘要，记录未执行的高风险分支；
6. 当前阶段只建立事实基线，不以虚构的全库百分比作为退出门禁；
7. 后续被修改文件执行“覆盖不下降”棘轮。

## 3. 必须验证的负向不变量

- 仅 include 某头文件不能产生行覆盖；
- 测试链接 stub 不能替代真实生产实现覆盖；
- 对目标分支做受控突变时，characterization 测试必须失败；
- coverage 构建不得改变普通 `build/` 的编译选项。

## 4. 实施结果

### 4.1 产物

- `src/CMakeLists.txt`、`src/test/CMakeLists.txt`：增加默认关闭的
  `AIAPI_ENABLE_COVERAGE`；
- `tools/coverage/generate_report.py`：直接解析 GCC 12 的压缩 gcov JSON；
- [`P01-runtime-coverage-report.md`](./P01-runtime-coverage-report.md)：机器生成的可读报告；
- `build-coverage/coverage-report.json`：本地机器报告，不进入 git；
- `.gitignore`：隔离 `build-coverage/`。

### 4.2 实际命令

```bash
cmake -S . -B build-coverage \
  -DAIAPI_BUILD_TESTS=ON \
  -DAIAPI_ENABLE_COVERAGE=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-coverage -j2
ctest --test-dir build-coverage --output-on-failure
python3 tools/coverage/generate_report.py

# 验证普通构建未被 coverage flags 污染
grep -R -- '--coverage\|-fprofile-instr-generate\|-fcoverage-mapping' \
  build/src/CMakeFiles build/src/test/CMakeFiles
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

### 4.3 事实基线

下表是 **P1-W1 完成时**的首次基线；机器报告会在 P1 后续工作项增加真实执行路径后持续
重生成，因此其当前数字应高于下表，不能反向覆盖这份起点记录。

| 项 | 结果 |
|---|---:|
| coverage 全量测试 | 223/223 PASS |
| 普通构建全量测试 | 223/223 PASS |
| production 实现文件 | 65 |
| 已编入 `aiapi_test` | 38 |
| 未编入 `aiapi_test` | 27 |
| instrument 实现行覆盖 | 3628/8526（42.55%） |
| instrument 实现分支覆盖 | 5169/25189（20.52%） |
| 普通构建 coverage flag 命中 | 0 |
| coverage 构建 flag 命中 | production/test 各 1 |

工具版本：`gcov (Debian 12.2.0-14+deb12u1) 12.2.0`、
`Python 3.11.2`。

百分比的分母只包含已 instrument 的实现文件；27 个未编译文件明确列为
`not_instrumented`，没有用 0 行或 include 可达性制造全库百分比。

### 4.4 高风险缺口

| 路径 | 当前证据 | P1 后续工作项 |
|---|---|---|
| `chaynsapi::generate/postChatMessage` | 未编入测试 target | P1-W2 假上游与四入口 fixture |
| ToolBridge `transform/emit` 权威成员 | 未编入测试 target | P1-W4 production characterization |
| Account 选择/失效/池重建 | 部分执行 | P1-W4 补 `getEligibleAccount` 等分支 |
| Account 注册失败回滚 | `rollbackWaitingAccount` 0 次 | P1-W4 store/http fake |
| `BackgroundTaskQueue` shutdown/drain | 已执行；函数行覆盖 69%～100% | P1-W5 堵塞/积压/SIGTERM harness |
| Chat/Responses Controller 四入口 | 未编入测试 target；只有 sinks 被测 | P1-W2/P1-W4 入口冒烟 |
| Retool workflow/agent | 未编入测试 target | P1-W3 fixture |
| `main` 停机编排 | 未编入测试 target | P1-W5 SIGTERM harness |

### 4.5 负向不变量结论

- **include 不等于覆盖**：报告只读取运行后 gcda；未编译的 Chayns、Generation、
  Controller 仍明确显示 `not_instrumented`；
- **stub 不等于真身执行**：被链接但没有执行的 DB/Session 实现显示
  `instrumented_not_executed`；
- **普通构建隔离**：普通 `build/` 中 coverage flags 命中数为 0；
- **受控突变**：属于 P1-W4 的阶段级门禁，本工作项没有提前声称通过。

## 5. 回滚

删除 coverage 构建选项和独立脚本即可；`AIAPI_ENABLE_COVERAGE` 默认关闭，
不修改生产运行配置、数据库或协议行为。删除本地 `build-coverage/` 不影响普通构建。
