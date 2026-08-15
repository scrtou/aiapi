# ADR-02 用 CMake target 与架构规则共同强制边界

| 项 | 值 |
|---|---|
| 状态 | 已接受，已实施（P8-W1） |
| 当前版本 | v4.0 |

## 决策

正式 target DAG 为：

```text
aiapi_platform
aiapi_domain          -> aiapi_platform
aiapi_application     -> aiapi_domain, aiapi_platform
aiapi_infrastructure  -> aiapi_domain, aiapi_platform, third-party IO libraries
aiapi_transport       -> aiapi_application, aiapi_domain, aiapi_platform, Drogon
aiapi_runtime         -> aiapi_application, aiapi_infrastructure, aiapi_transport
aiapi                 -> aiapi_runtime
```

`aiapi_application` 仅有 JsonCpp 的私有 compatibility-codec 链接，不接收 Drogon 的 usage
requirements；Drogon HTTP/DB/loop 类型仍由 ADR-09 的 source-closure gate 约束。最终可执行文件以
whole-archive 保留 `aiapi_transport` 的 Drogon 静态 Controller 注册对象，这不创建内层反向依赖。

测试只链接生产 library target，不在 `src/test/CMakeLists.txt` 复制生产 `.cpp` 清单。

## 为什么必须有两道门

ADR-03 的单一 `src/` include 根让源码能看见全部自有头。`target_link_libraries` 能约束链接和 usage
requirements，却不能独自阻止 domain/application include 一个自包含的 infrastructure 头；因此 CMake
DAG 必须和 `check_cycles.py --layer-rules`、ADR-09 boundary gate 一起运行。

## 强制措施

1. 禁止全局 `include_directories()`；
2. 每个自有 target 只暴露仓库 `src/` include 根；
3. target 只能按上图链接；
4. 每个 production implementation 恰有一个 CMake owner；
5. 测试只链接正式 target，并检查 CTest 注册；
6. `check_target_layers.py --require-no-legacy` 和
   `check_source_ownership.py --require-no-legacy` 拒绝 `aiapi_legacy` target/source list 复活；
7. layer rules、cycles、db include ratchet、startup wiring 和 HTTP/IO boundary 在 CI 独立执行。

## 验收

干净构建和测试通过、DAG/layer rules 无违反、无 legacy owner、测试不重新编译生产源码，且
architecture gate 的 selftest 能以受控 mutation 失败。基线只允许由工具从干净 commit 生成。
