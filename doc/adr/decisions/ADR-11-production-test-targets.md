# ADR-11 生产库与测试 target

| 项 | 值 |
|---|---|
| 状态 | 已接受，迁移中（唯一 owner 和六个正式 target 已建立） |
| 关联 | ADR-02、ADR-03 |
| 当前版本 | v2.0 |

## 决策

每个生产源文件只能属于一个 production target。主程序和测试通过链接生产库复用同一实现；禁止
测试 `CMakeLists.txt` 维护 `PROJECT_SOURCES` 复制清单，禁止把目录名作为 source 输入。

目标 DAG 以 ADR-02 为准，不是线性链：

```text
domain         -> platform
application    -> domain, platform
infrastructure -> domain, platform, third-party IO
transport      -> application, domain, platform, Drogon
runtime        -> application, infrastructure, transport
aiapi          -> runtime
```

测试按层链接最小 target，并通过 fake port 注入依赖。

迁移采用 strangler 方式：只有依赖方向已经合法的可编译闭包才能从 `aiapi_legacy`
迁入正式 target。被 domain JsonCpp、service locator、Provider session 副作用阻断的源码继续由
唯一 legacy owner 持有，数量由 CI ceiling 单调递减；不得整批改名为 runtime，也不得用
OBJECT library/link group 隐藏反向边。阶段 8 必须以 `--require-no-legacy` 删除该脚手架。

## 门禁

- CMake configure 失败于重复 source owner；
- test target 不得编译生产 `.cpp`；
- clean build 后运行时测试与主程序使用相同符号；
- architecture audit 记录每个 target 的 source owner 和依赖方向。
- `check_target_layers.py` 强制正式 target DAG 和 legacy ceiling；最终模式拒绝 legacy。
