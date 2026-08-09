# P3-W4 · domain 模型与 JSON codec 分离

| 项 | 值 |
|---|---|
| 状态 | DONE |
| 前置 | P3-W3 六个正式 target、application 首个真实闭包、legacy ceiling 39 |
| 目标 | `src/domain/` 中 JsonCpp/Drogon/DB/加密驱动类型归零，codec 归 edge owner |
| 决策 | [ADR-01](../decisions/ADR-01-layered-architecture.md)、[ADR-10](../decisions/ADR-10-domain-model-codec-boundary.md) |

## 1. 开工 inventory 与收口结果

直接命中 JsonCpp 的 domain 文件，全部完成：

| 顺序 | 文件 | 原 JSON 职责 | 落地 codec owner | 状态 |
|---:|---|---|---|---|
| 1 | `model/ProviderResult.h` | `meta: Json::Value` | `sessionManager/core/ProviderResultCodec.h` | DONE |
| 2 | `model/AccountData.h` | `fromJson/toJson` | `accountManager/AccountJsonCodec.h` | DONE |
| 3 | `model/ChannelInfo.h` | `fromJson/toJson` | `controllers/codecs/ChannelJsonCodec.h` | DONE |
| 4 | `model/RetoolWorkspaceInfo.h` | HTTP JSON、secret、extra/notes | `retoolWorkspace/RetoolWorkspaceJsonCodec.h` | DONE |
| 5 | `port/APIinterface.h` | `getModels(): Json::Value` | `controllers/codecs/ProviderModelCatalogJsonCodec.h` | DONE |
| 6 | `model/ErrorEvent.h` | detail 和 `toJson` | `dbManager/metrics/ErrorEventJsonCodec.h` | DONE |
| 7 | `model/SessionData.h` | tools、response、clientInfo、history | `sessionManager/contracts/LegacySessionData.h` + 既有 `SessionCodec.h` | DONE |

收口口径（本文件写入时实测）：

```text
grep -R "Json::\|json/json.h" src/domain    -> 0 命中
grep -Rl "drogon\|libpq\|pqxx\|openssl" src/domain -> 无
```

没有使用 typedef、前置声明或 `void*`/`std::any` 规避 grep：七个模型的 JSON 字段
要么替换为标准库 value（`map<string,string>`、`ErrorDetails` variant map、
强类型 struct），要么整块搬到边缘 contract。

## 2. 分层结果

- `port/APIinterface.h` 的 `getModels()` 改为返回 `ProviderModelCatalog`，宽端口本身的
  删除仍属 P6，本阶段只消除显式 `Json::Value` 返回类型；
- `session_st` 拆为 domain 的 `SessionState` 与 `sessionManager/contracts/LegacySessionData.h`
  中的过渡聚合；后者的 `Legacy` 前缀是刻意标记，P7 pipeline 迁移时随 edge 一并处理；
- `ProviderResultCodec.h` / `LegacySessionData.h` 都归当前 legacy generation edge 所有，
  不构成第二套 Provider 接口，未提前触碰 P6/P7 范围。

## 3. 契约测试

每个迁出 codec 均有 round-trip、缺省值、未知字段与敏感字段行为的直接用例：

- account/channel/workspace：round-trip 等价、未知字段保留、secret 不出现在日志面；
- provider model catalog：目录排序与空目录；
- ErrorEvent：四种标量往返、null 输入归空、嵌套值降级为显式字符串；
- ProviderResult：round-trip、空 metadata、非字符串拒绝。

全量：ctest 273/273 PASS（P3-W4 期间净增 12 条），六项架构门禁 PASS。

## 4. 退出门禁（实测）

- `src/domain/` 中 `Json::`、`json/json.h`、Drogon、PostgreSQL、OpenSSL 命中均为 0；✅
- domain target 只链接 platform；✅（`check_target_layers` PASS）
- 每个迁出 codec 有 round-trip/缺省/未知/secret 契约测试；✅
- legacy ceiling 未回升（仍为 39）；✅
- normal 全量测试与 `check_cycles`/`check_source_ownership`/`check_target_layers`/
  `check_include_paths`/`check_startup_wiring`/`check_test_registration` 全部通过。✅

## 5. 回滚

每个模型单独提交（C1–C8），失败时按单个 commit 回滚该模型 value、codec 与全部调用点；
不保留 domain/edge 双 codec，不得通过重新给 domain 链 JsonCpp 恢复构建。
