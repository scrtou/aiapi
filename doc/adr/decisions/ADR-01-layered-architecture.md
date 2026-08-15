# ADR-01 采用端口与适配器架构

| 项 | 值 |
|---|---|
| 状态 | 已接受，已实施（P8-W1） |
| 当前版本 | v4.0 |

## 决策

项目按端口与适配器划分；它们不是编号式的线性层：

```text
transport ───────> application ───────> domain
      │                                      ▲
      └──────── composition root ────────────┤
infrastructure ──────────────────────────────┘

platform <── domain/application/infrastructure/transport
```

- `domain`：业务值类型、纯规则和 ports；
- `application`：用例与流程编排，只通过 port 发起所需 IO；
- `infrastructure`：DB、Provider、HTTP、clock 等 port 实现；
- `transport`：Drogon Controller、Filter、协议编解码和 Sink；
- `platform`：`Result`、错误、日志、时间/并发值对象等无业务方向设施；
- `runtime/AppContext/main.cc`：唯一能同时知道具体实现的组合根。

适配器依赖内侧 port，内侧不依赖具体适配器。目录名不是边界证明；正式 CMake target、include/layer
规则和 source-owner gate 才是可执行边界。

## Domain 依赖政策

`aiapi_domain` 只允许 C++ 标准库和 `aiapi_platform`。`src/domain/` 中 `Json::`、Drogon、
PostgreSQL、OpenSSL、具体 Provider/DbManager/Controller 类型均为 0。

JSON 可以在边缘 codec 使用。P8 保留的 application 请求适配入口只接收 transport 已复制的
`Json::Value` 与 `RequestHeaders`，随后构造结构化 request；它不把 Drogon request、HTTP client 或
JSON 类型带入 domain。Provider metadata 以 string map 表示，JSON materialization 留在边缘。

## 实施与验收

- 六个正式 target 已接管全部 89 个 production implementation；`aiapi_legacy` 和所有 transition
  allowlist 已删除；
- `check_target_layers.py --require-no-legacy`、`check_source_ownership.py --require-no-legacy` 与
  layer/include/cycle gates 强制依赖方向；
- `check_http_io_boundary.py` 扫描 application include closure 与 domain，拒绝 Drogon/Trantor、
  `HttpClient`、`DbClient` 和直接 `sleep_for`；
- `AppContext` 显式拥有状态对象，Controller 只绑定 use case，具体 adapter 只在 runtime 接线。

边缘 JSON codec 必须有契约或往返测试；真实执行覆盖仍以 gcov/llvm-cov 而非 include 关系判断。
