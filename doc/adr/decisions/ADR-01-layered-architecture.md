# ADR-01 采用端口与适配器架构

| 项 | 值 |
|---|---|
| 状态 | 已接受，迁移中 |
| 当前版本 | v3.0 |

## 决策

项目划分为四类组件和一个横切基础 target。它们不是按编号排列的线性层级：

```text
transport ───────> application ───────> domain
      │                                      ▲
      └──────── composition root ────────────┤
infrastructure ──────────────────────────────┘

platform <── domain/application/infrastructure/transport
```

- `domain`：业务值类型、纯规则和 ports。
- `application`：用例与流程编排；可以调用 port 触发 IO，但不知道具体 DB、HTTP 或 Drogon 类型。
- `infrastructure`：DB、Provider、HTTP、时钟等 port 实现。
- `transport`：Drogon Controller、Filter、协议编解码和 Sink。
- `platform`：`Result`、日志抽象、进程配置值等无业务方向的公共设施。
- `AppContext/main.cc` 是唯一允许同时知道所有具体实现的组合根。

依赖规则是：**适配器依赖内侧端口，内侧不依赖适配器**。

## Domain 依赖政策

目标态 `aiapi_domain` 只允许 C++ 标准库和 `aiapi_platform`，禁止直接依赖 JsonCpp、Drogon、PostgreSQL、OpenSSL 及任何具体 Provider/DbManager/Controller。

当前 `src/domain/model/` 中的 JsonCpp 依赖是迁移债务，不是永久豁免：

1. domain 类型只保留强类型字段；
2. `fromJson()/toJson()` 移到 transport 或 infrastructure codec；
3. 暂时无法迁出的依赖登记到 `tools/arch/layer-rules.json` 的有上限债务清单；
4. 清单只能减少，最终归零。

“domain 无 IO”的准确含义是 domain 不直接执行具体 IO，也不包含协议/驱动类型；不是说 application 不能经 port 发起 IO。

## 验收

- `aiapi_domain` 可独立编译和运行单元测试；
- domain 中 `Json::`、Drogon、PostgreSQL、OpenSSL 引用为 0；
- 依赖方向由 ADR-02 的 target DAG 和架构规则共同检查；
- 边缘 JSON codec 有往返或契约测试。
