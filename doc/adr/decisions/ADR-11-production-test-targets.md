# ADR-11 生产库与测试 target

| 项 | 值 |
|---|---|
| 状态 | 已接受，已实施（P8-W1） |
| 关联 | ADR-02、ADR-03 |
| 当前版本 | v3.0 |

## 决策

每个 production source 文件只能属于一个 production target。主程序和测试通过链接生产库复用同一实现；
禁止测试 `CMakeLists.txt` 维护 `PROJECT_SOURCES` 复制清单，禁止把目录名作为 source 输入。

目标 DAG：

```text
domain         -> platform
application    -> domain, platform
infrastructure -> domain, platform, third-party IO
transport      -> application, domain, platform, Drogon
runtime        -> application, infrastructure, transport
aiapi          -> runtime
```

测试按层链接最小 target，并通过 fake port 注入依赖。`aiapi_test` 与 SIGTERM fixture 都从正式 target
获得 production symbols；不允许 standalone `src/test` 配置绕过此规则。

## 已实施收口

P8 已将最后 19 个 legacy owner 分配到正式 target，删除 `aiapi_legacy` target/source list 及其过渡
ceiling。当前所有 89 个 production implementation 只有一个 owner；`main.cc` 是最终 executable 的
唯一 source。`check_source_ownership.py --require-no-legacy` 还可结合 compile commands 验证每个
translation unit 只编译一次。

## 门禁

- CMake configure 对重复 source owner 失败；
- `check_source_ownership.py --require-no-legacy` 拒绝重复 owner、测试侧 production source 清单和 legacy；
- `check_target_layers.py --require-no-legacy` 强制正式 target DAG，拒绝 legacy target/source list；
- `check_test_registration.py --require-strict` 在配置后的 build 中核对 `TEST_SOURCES`、Drogon cases 与
  CTest 注册；
- architecture audit 记录 owner/依赖规模，发布 baseline 只能来自干净 commit。
