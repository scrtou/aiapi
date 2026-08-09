# ADR-10 领域模型与 JSON/DB codec 边界

| 项 | 值 |
|---|---|
| 状态 | 已接受，待实施 |
| 关联 | ADR-01、ADR-02、ADR-05 |

## 决策

领域模型只表达业务值和不变量，不提供 `fromJson/toJson`，不包含 JsonCpp、Drogon、OpenSSL 或
SQL 类型。HTTP JSON、上游 JSON 和 DB snapshot 必须分别拥有 codec：

```text
transport codec  -> application command
provider codec   -> ProviderRequest/ProviderResponse
store codec      -> domain value/snapshot
```

协议未知字段放在 edge-owned `JsonDocument`，不得把 `Json::Value` 作为跨层通用逃生舱。公开响应
的 snake/camel 命名只在 transport codec 决定；密钥、Cookie 和密码使用独立 secret 类型，默认不可
序列化到公开视图。

## 迁移顺序

ProviderResult/SessionData → AccountData/ChannelInfo → RetoolWorkspaceInfo/ErrorEvent。每个模型先
建立 codec 往返、未知字段、缺省值和敏感字段测试，再删除模型内 JSON 方法。

## 验收

`architecture_audit` 对 domain 的第三方 include/类型命中为 0；application 只使用结构化值；
所有现有 HTTP/DB golden fixture 仍通过语义比较。
