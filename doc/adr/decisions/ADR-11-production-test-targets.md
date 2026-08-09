# ADR-11 生产库与测试 target

| 项 | 值 |
|---|---|
| 状态 | 已接受，部分实施（P3-W1 已建立唯一 owner，正式分层 target 待 P3-W3） |
| 关联 | ADR-02、ADR-03 |

## 决策

每个生产源文件只能属于一个 production target。主程序和测试通过链接生产库复用同一实现；禁止
测试 `CMakeLists.txt` 维护 `PROJECT_SOURCES` 复制清单，禁止把目录名作为 source 输入。

目标 DAG 为 `aiapi_platform → aiapi_domain → aiapi_application → aiapi_infrastructure → aiapi_transport
→ aiapi_runtime → aiapi`。测试按层链接最小 target，并通过 fake port 注入依赖。

## 门禁

- CMake configure 失败于重复 source owner；
- test target 不得编译生产 `.cpp`；
- clean build 后运行时测试与主程序使用相同符号；
- architecture audit 记录每个 target 的 source owner 和依赖方向。
