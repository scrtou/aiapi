# P02 Provider 代码退役与 tombstone

> 工作项：P2-W2<br>
> 状态：DONE<br>
> retirement_id：`retire-nexos-openai-v1`<br>
> 前置产物：[`P02-provider-data-retirement.md`](P02-provider-data-retirement.md)

## 1. 输入、范围和不可变边界

本工作项只执行阶段 2 的代码侧退役，遵循：

```text
migration-plan → refactor-workbook → work-products → 阶段门禁 → 提交
```

不可变边界：

1. 活跃 Provider 只保留 `chaynsapi`、`retoolapi`；
2. 删除具体 Nexos Provider 和 `OpenAiProvider`；
3. 保留公开的 OpenAI Chat Completions/Responses **协议**；
4. 保留 Retool `openai_resource_*` 等合法业务字段；
5. 不在代码退役中删除历史指标、`account_backup` 或归档快照；
6. 代码回滚不代替数据恢复，数据恢复也不代替代码回滚。

## 2. 删除与保留清单

### 2.1 删除

| 类别 | 内容 |
|---|---|
| Provider 实现 | `src/apipoint/openai/OpenAiProvider.{h,cpp}` |
| Provider 实现 | `src/apipoint/nexosapi/nexosapi.{h,cpp}` |
| Nexos 辅助类 | `NexosRegistrationMailPolicy.h`、`NexosUserAgent.h` |
| 专属测试 | 两个 Nexos 辅助类测试 |
| 注册 | production/test CMake 中的源文件、include 路径 |
| 账号逻辑 | Nexos token 登录/校验/更新、Nexos 注册分支、预算耗尽专属归档分支 |
| 配置样例 | `providers.openai/nexos`、`outbound_limits.openai/nexosapi`、Nexos 下游 key、tool-bridge retired channel 项 |
| 通道默认值 | Nexos 内置通道及 active built-in 白名单 |

### 2.2 明确保留

| 内容 | 原因 |
|---|---|
| chayns/retool Chat Completions 路由 | 公开 OpenAI-compatible 协议 |
| chayns/retool Responses create/get/delete 路由 | 公开 OpenAI-compatible 协议 |
| Retool `openai_resource_uuid/name` | Retool 资源绑定业务字段，不是被退役的 Provider |
| `accountBackupDbManager` 和 backup info API | 历史备份数据不属于本工作项删除范围 |
| `error_agg_*` 历史事件 | 保留历史审计；active status 查询排除 retired provider 维度 |
| 六条 Nexos 路由 | 发布期 tombstone；满足删除条件后另行移除 |

## 3. 运行时调用图

### 3.1 历史路由 tombstone

```text
Drogon route
  → AiApiController::retiredNexos[WithId]
    → retired_provider::respondNexosTombstone
      ├→ makeNexosTombstoneMetric
      ├→ ErrorStatsService::recordWarn
      └→ HttpResponse(410 + stable JSON + retirement header)
```

handler 不进入 `ApiManager`、账号池、session pipeline 或上游 HTTP；因此 tombstone 调用不会触发上游副作用。

### 3.2 配置启动失败

```text
main config load
  → ConfigValidator::validate
    → RetiredProviderPolicy
      → 返回包含精确 JSON 路径的 validation errors
        → 启动失败，不注册 retired provider/channel/account
```

### 3.3 写入侧保护

```text
AccountController / background automation
  → AccountManager::{addAccount, addAccountbyPost, updateAccount, autoRegisterAccount}
    → RetiredProviderPolicy → reject before memory/store/upstream

ChannelController
  → explicit 410/provider_retired
  → ChannelManager::{addChannel, updateChannel, updateChannelStatus}
    → RetiredProviderPolicy → reject before store
```

数据库加载时，AccountManager/ChannelManager 也拒绝把遗留 retired 行放回运行时索引，并提示先执行 `retire_providers_v1.sql`。

## 4. tombstone 契约矩阵

六条路由均返回同一契约：HTTP `410`，header `X-AIAPI-Retirement-Id: retire-nexos-openai-v1`，body `error.code=provider_retired`，并列出 `chaynsapi/retoolapi` 替代项。

| 方法 | 历史路径 | handler |
|---|---|---|
| POST | `/nexosapi/v1/chat/completions` | `retiredNexos` |
| GET | `/nexosapi/v1/models` | `retiredNexos` |
| GET | `/nexosapi/v1/account/quota` | `retiredNexos` |
| POST | `/nexosapi/v1/responses` | `retiredNexos` |
| GET | `/nexosapi/v1/responses/{id}` | `retiredNexosWithId` |
| DELETE | `/nexosapi/v1/responses/{id}` | `retiredNexosWithId` |

指标事件为 `provider.retired_route_called`。detail 只包含 retirement id、route family、path、method、replacement providers；不采集 Authorization 或 body。事件的 provider 维度留空，避免把 retired key 重新写入 active request aggregate。

## 5. 旧配置错误矩阵

| 旧配置位置 | 处理 |
|---|---|
| `custom_config.providers.openai/nexos/nexosapi` | 启动拒绝并返回精确键名 |
| `custom_config.outbound_limits.openai/nexosapi` | 同上 |
| `login_service_urls[i].name` | retired name 时拒绝 |
| `regist_service_urls[i].name` | retired name 时拒绝 |
| `downstream_service_api_keys[i].name` | retired name 时拒绝 |
| `account[i].apiname` | retired name 时拒绝 |
| `tool_bridge.format_by_channel.<key>` | retired key 时拒绝 |
| `tool_bridge.strict_sentinel_by_channel.<key>` | retired key 时拒绝 |
| tool-bridge enabled/disabled channel lists | retired value 时拒绝并返回数组索引 |

校验按完整键匹配，不进行 `openai` 子串扫描；`openai_resource_uuid` 等合法字段不会误伤。

## 6. 指标和 Dashboard 边界

- 新 tombstone 只写 warning event，provider 维度为空；
- `StatusDbManager` 的 channel/model active status 查询排除两个 retired provider key；
- 历史 error events 不删除，仍可用于审计；
- 外部 Dashboard、告警规则和部署平台标签不在仓库内，发布负责人必须在部署变更中移除 retired provider 选项；本报告不把该外部动作伪造成已完成。

## 7. 自动精确门禁

新增：

```bash
python3 tools/arch/check_retired_providers.py
```

它检查：两个实现目录不存在、具体类/工厂注册为零、内置白名单精确为 chayns/retool、六条路由全部进入 tombstone、样例配置无旧键、chayns/retool Chat/Responses 路由仍在、Retool 合法业务字段仍在。禁止用笼统的 `grep -ri openai src` 作为结论。

## 8. 验证记录

最终结果（全部离线，未访问真实上游）：

| 门禁 | 结果 |
|---|---|
| normal build + CTest | 260/260 PASS，9.43s |
| strict test registration | 260 声明 / 260 注册，完全一致 |
| coverage CTest | 260/260 PASS，36.07s |
| coverage report | 行 6036/11909（50.68%），分支 9218/35499（25.97%） |
| ASan（`detect_leaks=0:halt_on_error=1`） | 260/260 PASS，16.39s |
| provider retirement migration fixture | preflight/retire/idempotence/restore/rollback/conflicts PASS |
| retirement precise gate | PASS |
| architecture audit selftest + baseline ratchet | PASS；R1=0、R3=13，无回归 |
| cycle + layer gate | 0 cycle，0 layer violation，PASS |
| startup wiring | PASS |
| `git diff --check` | PASS |

核心命令：

```bash
cmake -S . -B build && cmake --build build -j2
ctest --test-dir build --output-on-failure
python3 tools/arch/check_test_registration.py --require-strict

cmake -S . -B build-coverage -DAIAPI_ENABLE_COVERAGE=ON
cmake --build build-coverage -j2
find build-coverage -name '*.gcda' -delete
ctest --test-dir build-coverage --output-on-failure
python3 tools/coverage/generate_report.py

cmake -S . -B build-asan \
  -DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address'
cmake --build build-asan -j2
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure

python3 tools/migrations/test_provider_retirement.py
python3 tools/arch/check_retired_providers.py
python3 tools/architecture_audit.py --selftest
python3 tools/architecture_audit.py --baseline doc/adr/audits/audit-baseline.json
python3 tools/arch/check_cycles.py \
  --baseline tools/arch/cycles-baseline.json \
  --layer-rules tools/arch/layer-rules.json
python3 tools/arch/check_startup_wiring.py
```

代码回滚与数据恢复分别验证：数据脚本 fixture 已实际执行 archive/delete/restore 和冲突 rollback；代码侧在暂存完整变更后用 `git diff --cached --binary | git apply --reverse --check` 验证反向补丁可应用。该检查证明代码提交可机械回退，但不冒充旧代码与已归档生产数据的联合上线演练。

## 9. 回滚与 tombstone 删除条件

### 9.1 代码回滚

回滚本工作项提交，恢复 Provider 源码、注册和配置解析。代码回滚前必须确认数据状态；如果数据已归档，旧代码不会因为源码恢复而自动获得账号/渠道数据。

### 9.2 数据恢复

按 `P02-provider-data-retirement.md` 执行 `restore_retired_providers_v1.sql`。恢复脚本遇主键/唯一键冲突整批失败，不能静默覆盖。数据恢复后仍需部署与之匹配的旧代码版本。

### 9.3 tombstone 删除

只有同时满足以下条件才可建立后续删除工作项：

1. 本版本已经跨过一个成功发布边界；
2. 观察窗口内 `provider.retired_route_called` 为 0；
3. 外部 Dashboard/告警/调用方已经清理；
4. 另有显式变更和回滚方案。

本工作项不提前删除 tombstone。
