# P2-W1 · Provider 数据预检、归档与恢复

| 项 | 值 |
|---|---|
| 状态 | DONE（目标环境仅交付只读执行清单；当前没有运行中的部署或挂载业务库） |
| 范围 | 只处理退役 provider key：`nexosapi`、`openai` |
| 当前数据库方言 | SQLite 3（`config.json` 的 `aichatpg`） |
| 生产数据操作 | 尚未执行；不得把 fixture 或 `build/data` 称为目标部署证据 |
| 前置 | P1-W1～P1-W5 已完成并提交（`324e7f6`） |

## 1. 输入与真实 schema 修正

计划中的 `accounts.api_name` 是概念示例，不是当前 schema。源码建表语句与本地 SQLite
schema 对账后的真实字段是：

```text
account.apiname                    主键 id
channel.channelname/channeltype    主键 id；channelname UNIQUE
chat_session_state.api_name        主键 session_id
```

当前 schema 没有这三张表之间的数据库外键；`account.apiname → channel.channelname` 是应用层
语义关联。归档不能据此省略关联表审计。

## 2. 数据分类与处理策略

| 表/数据 | 与退役 Provider 的关系 | P2-W1 策略 |
|---|---|---|
| `account` | nexos 账号；也预检可能存在的 openai 账号 | 完整快照后删除，支持原主键恢复 |
| `channel` | 内置 `nexosapi`；可能存在 openai channel | 完整快照后删除，恢复时同时检查 id 与唯一 channelname 冲突 |
| `chat_session_state` | `api_name` 选择 provider，payload 可能含 provider 状态 | 完整快照后删除，支持 session_id 恢复 |
| `response_index` | 通过 session_id 关联，但本身是公开响应连续性索引 | 保留；无外键，不主动破坏公开响应索引 |
| `error_event/error_agg_hour/request_agg_hour` | provider 维度历史审计/指标 | 保留历史，只在 dry-run 计数；P2-W2 清理展示/告警维度 |
| `account_backup` | 独立 SQLite 文件中的历史备份 | 保留；部署预检需单独确认文件与行数 |
| `retool_workspace.openai_resource_*` | Retool workspace 的资源字段，不等于待删 OpenAiProvider | 明确保留，防止误删合法 `openai*` 业务字段 |

## 3. 迁移设计

固定 migration/retirement id 为 `retire-nexos-openai-v1`，使同一版本重复执行可判定为
幂等 no-op，而不是每次产生新的空归档批次。计划交付：

```text
tools/migrations/provider_retirement_preflight_v1.sql
tools/migrations/retire_providers_v1.sql
tools/migrations/restore_retired_providers_v1.sql
tools/migrations/test_provider_retirement.py
```

归档采用同库事务表：每条记录保存 `retirement_id`、source table、文本化原主键、provider
key 和包含全部当前列的 JSON snapshot。snapshot 含凭据，权限、备份和传输等级必须与原业务库
相同，禁止写入日志或工作产物。

已完成并验证：

- archive/live 逐主键且逐 canonical JSON snapshot 对账后才允许删除；
- 删除数、剩余目标数与 marker 数字一致，否则 guard 失败，CLI 关闭连接并 rollback；
- schema guard 校验精确列集合；允许历史 `ALTER TABLE` 造成的 cid 顺序差异，但遇新增、
  缺失或重命名列会停止，防止“不完整快照”继续运行；
- 已 applied 且 live 无漂移时重复执行为持久状态 no-op；若目标 live row 重新出现则停止；
- restore 前检查 account/channel/session 原主键，并额外检查 `channel.channelname` UNIQUE
  冲突；任一冲突整批终止，脚本从不使用 replace/update 覆盖 live row；
- restore 后用相同 JSON 字段顺序逐行对账，再把 marker 切换为 `restored`；
- fixture 完整往返、幂等、归档不一致 rollback、重复恢复冲突、主键冲突和唯一键冲突
  均已自动演练；
- 对目标部署数据库只运行 preflight/副本 dry-run，未经备份和维护窗口批准不改原库。

### 3.1 状态机与调用顺序

```text
retire_providers_v1.sql
  ├─ BEGIN IMMEDIATE
  ├─ 建 archive/marker（IF NOT EXISTS）
  ├─ schema exact-column-set guard
  ├─ 读取 marker prior_status + 统计 live candidates
  ├─ INSERT archive ... ON CONFLICT DO NOTHING
  ├─ PK + 完整 snapshot + archive 总数对账
  ├─ DELETE account/channel/chat_session_state
  ├─ deleted/remaining 对账
  ├─ 写入或更新 marker(status=applied)
  └─ COMMIT

restore_retired_providers_v1.sql
  ├─ BEGIN IMMEDIATE
  ├─ schema/marker/archive 总数 guard
  ├─ account.id/channel.id/session_id/channelname 冲突 guard
  ├─ 从 JSON snapshot INSERT（无 OR REPLACE、无 UPSERT）
  ├─ live row 与 archive snapshot 逐行对账
  ├─ marker(status=restored)
  └─ COMMIT
```

`retirement_id` 固定不等于允许覆盖：恢复后若 live row 与原 snapshot 不同，再次 retire 会
因 snapshot mismatch 停止，要求运维明确决定新 migration id，而不是丢失恢复后的修改。

## 4. 自动 fixture 与副本演练

执行：

```bash
python3 tools/migrations/test_provider_retirement.py
```

fixture 使用当前三张表的完整字段，并故意采用历史升级库中
`status/workspaceuacid`、`supports_tool_calls/accountretentiondays` 的 cid 顺序。数据矩阵为：

```text
account:            nexosapi + openai + control
channel:            channelname=nexosapi + custom-name/channeltype=openai + control
chat_session_state: nexosapi + openai + control
retained:           response_index + 三张 metrics + retool openai_resource fields
```

最终结果：

```text
PASS provider retirement fixture: preflight, retire, idempotence, restore, rollback, conflicts
```

已验证的失败路径：

| 场景 | 结果 |
|---|---|
| applied 后重复 retire | archive/marker 内容与时间戳不变 |
| restored 后重复 restore | 主键冲突 guard 非 0 退出，live 数据不变 |
| restored row 修改后再次 retire | snapshot guard 非 0 退出，6/6 target 均未删除 |
| account 原主键被其他 provider 占用 | restore 非 0，其他表无部分恢复 |
| channelname 被不同 id 占用 | restore 非 0，冲突 row 不被覆盖，其他表无部分恢复 |

对 `build/data/aiapi.db` **只复制到临时目录**后完成同脚本演练：本地副本候选数 3，
archive/delete/restore 均为 3，最终 marker 为 `restored (3/3/3)`；原文件 SHA-256 前缀
`88d79814142ab308` 在演练前后相同。该证据只证明脚本兼容当前本地升级 schema，不是生产
审批或生产 dry-run。

## 5. 目标部署 preflight 与执行清单

`build/data/aiapi.db` 是构建目录中的本地运行/测试数据库，不足以证明它就是目标部署库。
它只用于核对当前 SQLite schema；本报告没有记录账号、token、payload 或其他敏感字段，也
没有把其行数冒充生产 dry-run。当前 `docker ps` 没有 aiapi 容器，仓库 `./data` 也没有挂载的
业务库，因此不存在可被确认的在线目标数据库。

目标部署时先停在只读步骤：

```bash
# 1. 主库只读报告；输出只有 schema、计数和主键范围
sqlite3 /deployment/data/aiapi.db \
  < tools/migrations/provider_retirement_preflight_v1.sql \
  > provider-retirement-preflight.txt

# 2. 独立备份库关联检查；不输出账号字段
sqlite3 /deployment/data/account_backup.db \
  "PRAGMA query_only=ON;
   SELECT apiname,COUNT(*),MIN(id),MAX(id)
   FROM account_backup
   WHERE apiname IN ('nexosapi','openai') GROUP BY apiname;"

# 3. 在停服后一致性副本上演练，不直接对原库 retire
cp --reflink=auto /deployment/data/aiapi.db /approved-workdir/aiapi-retire-dry-run.db
sqlite3 /approved-workdir/aiapi-retire-dry-run.db \
  < tools/migrations/retire_providers_v1.sql
sqlite3 /approved-workdir/aiapi-retire-dry-run.db \
  < tools/migrations/restore_retired_providers_v1.sql
```

部署记录必须补：环境/主机匿名标识、SQLite 版本、`user_version`、文件 SHA-256、三张目标表
的脱敏计数与主键范围、关联表计数、备份位置、执行人/复核人和维护窗口。禁止记录 snapshot
内容，因为其中含 password/token/session payload。

只有副本往返与备份恢复都成功，才允许在维护窗口执行原库 retire。生产脚本执行后必须查询：

```sql
SELECT retirement_id,status,expected_rows,archived_rows,deleted_rows
FROM provider_retirement_migration_v1
WHERE retirement_id='retire-nexos-openai-v1';
```

四个数字/状态必须为 `applied, N, N, N`，且三张 live 表目标计数均为 0。

## 6. 方言和安全边界

- 两个迁移脚本仅适用于当前 `config.json` 声明的 SQLite；若目标环境实际使用 PostgreSQL/
  MySQL，P2-W2 前必须另做该方言脚本与副本演练，禁止“试着运行”SQLite 脚本；
- `.bail on` 是事务安全设计的一部分，脚本必须由 `sqlite3` CLI 执行；不应切割语句后逐条
  从 GUI 运行；
- archive 与原表处在同一库，保证原子性但不等于备份；库文件损坏会同时损坏 live/archive，
  因此外部只读备份是上线前置；
- archive snapshot 仍是敏感数据，权限与加密等级不能低于原库；
- metrics、Retool `openai_resource_*`、OpenAI 兼容协议字段明确不属于本数据删除范围。

## 7. 回滚

- 代码回滚与数据回滚分开：P2-W2 代码回退不自动恢复账号/渠道/会话；
- 数据恢复只使用 `restore_retired_providers_v1.sql`，且必须对应 marker 的同一
  `retirement_id`；
- 遇 live 主键/唯一键冲突时脚本必然停止。应先导出并人工判断冲突数据，禁止临时改成
  `INSERT OR REPLACE`；
- restore 成功后保留 archive 和 marker 供审计；不要删除归档表；
- 若脚本因 schema guard 失败，先更新迁移版本并重新跑 fixture/副本，不能删除 guard。
