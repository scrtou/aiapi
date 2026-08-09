# ADR-02 用 CMake target 与架构规则共同强制边界

| 项 | 值 |
|---|---|
| 状态 | 已接受，部分落地 |
| 当前版本 | v3.0 |

## 决策

```text
aiapi_platform
aiapi_domain          -> aiapi_platform
aiapi_application     -> aiapi_domain, aiapi_platform
aiapi_infrastructure  -> aiapi_domain, aiapi_platform, third-party IO libraries
aiapi_transport       -> aiapi_application, aiapi_domain, aiapi_platform, Drogon
aiapi                 -> 上述适配器 target（组合根）
```

测试链接这些 library target，不再在 `src/test/CMakeLists.txt` 复制生产 `.cpp` 清单。

## 为什么必须有两道门

ADR-03 的单一 include 根会让源码看见 `src/` 下所有头文件。因此 `target_link_libraries` 能约束链接和 usage requirements，但不能单独阻止 domain include 一个自包含的 infrastructure 头。必须同时运行 `tools/arch/check_cycles.py --layer-rules ...`。

禁止再声称“domain 不链接 Drogon，所以 include Drogon 会自动失败”。这取决于头文件可见性和符号使用，不是可靠边界。

## 强制措施

1. 禁止全局 `include_directories()`；
2. 每个自有 target 只暴露仓库 include 根；
3. target 只能按上述 DAG 链接；
4. layer rules 校验全部模块方向，而不只检查环；
5. CI 使用干净构建目录；
6. 测试链接生产 library，并校验测试注册一致性；
7. 新增源码未进入任何 target 时 CI 失败。

## 迁移与验收

按“先建立库、再逐模块搬入”迁移，每次只移动一个可编译闭包，不复制源码形成双轨。完成后删除单一 `add_executable` 的生产清单和测试侧 `PROJECT_SOURCES`。

验收要求：干净构建和测试通过、依赖图无环、layer rules 无新增豁免、测试不重新编译私有生产源码，且人为注入非法依赖时 CI 能失败。
