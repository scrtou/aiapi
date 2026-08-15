# Provider 退役数据运维

这组脚本只处理已退役的具体上游 Provider key：`nexosapi` 和 `openai`。它们**不**删除
OpenAI 兼容 HTTP 路由，也不删除 Retool Workspace 中合法的 `openai_resource_*` 业务字段。

## 范围和前提

- 当前脚本只支持仓库当前的 **SQLite** schema；目标环境若使用 PostgreSQL 或 MySQL，必须先交付
  对应方言的新脚本和副本演练，不能尝试运行这些 SQL。
- 需要 `sqlite3` CLI；脚本使用 `.bail on` 与单个事务，不能从 GUI 拆成单条语句执行。
- archive snapshot 包含账号凭据和 session payload。数据库、备份、导出和权限必须按敏感数据处理；
  不要把 preflight/SQL 输出、snapshot 或数据库副本提交到仓库。
- 固定批次 ID 是 `retire-nexos-openai-v1`。重复 retire 只允许在上次已 applied 且不存在重新出现的
  live row 时成为 no-op；发现 drift 时会停止。

## 文件

| 文件 | 用途 |
|---|---|
| `provider_retirement_preflight_v1.sql` | 只读 schema/候选数/关联维度报告；不选择凭据或 payload 内容。 |
| `retire_providers_v1.sql` | 归档 `account`、`channel`、`chat_session_state` 的完整 row snapshot，对账后删除 live row。 |
| `restore_retired_providers_v1.sql` | 从同一 archive batch 恢复；任何 source PK 或 `channelname` UNIQUE 冲突都会终止整个事务。 |
| `test_provider_retirement.py` | 离线 fixture：preflight、retire、幂等、restore、rollback 和冲突演练。 |

脚本不会删除 historical metrics、`response_index`，也不会触碰 Retool resource 字段。

## 发布步骤

从仓库根目录执行；将路径替换为经批准的维护窗口工作目录。

```bash
# 0. 先验证脚本本身（不接触部署数据库）
python3 tools/migrations/test_provider_retirement.py

# 1. 对原库只读 preflight；保存输出时按敏感运维记录处理
sqlite3 /deployment/data/aiapi.db \
  < tools/migrations/provider_retirement_preflight_v1.sql \
  > /secure-workdir/provider-retirement-preflight.txt

# 2. 创建外部备份并在一致性副本上完成完整往返
cp --reflink=auto /deployment/data/aiapi.db /secure-workdir/aiapi-retire-dry-run.db
sqlite3 /secure-workdir/aiapi-retire-dry-run.db \
  < tools/migrations/retire_providers_v1.sql
sqlite3 /secure-workdir/aiapi-retire-dry-run.db \
  < tools/migrations/restore_retired_providers_v1.sql

# 3. 仅在备份、preflight、审批和副本演练均完成后，对停服的原库 retire
sqlite3 /deployment/data/aiapi.db < tools/migrations/retire_providers_v1.sql
sqlite3 /deployment/data/aiapi.db \
  "SELECT retirement_id,status,expected_rows,archived_rows,deleted_rows
   FROM provider_retirement_migration_v1
   WHERE retirement_id='retire-nexos-openai-v1';"
```

成功 marker 必须是 `applied`，且三个计数相同；三个 live 表中目标 key 的候选数必须归零。
记录环境匿名标识、SQLite 版本、`user_version`、文件 hash、脱敏计数/主键范围、备份位置、执行人、
复核人和维护窗口；不要记录 row snapshot 内容。

## 恢复和回滚

代码回滚与数据恢复是两个独立动作。若需要恢复已归档数据，先停止写入并确认没有冲突：

```bash
sqlite3 /deployment/data/aiapi.db \
  < tools/migrations/restore_retired_providers_v1.sql
```

脚本刻意不使用 `INSERT OR REPLACE` 或 UPSERT。PK/`channelname` 冲突、archive 对账失败和 schema
漂移都会使整个事务回滚；请导出并人工裁决冲突，不能通过修改脚本来覆盖 live 数据。

完整 schema 理由、fixture 矩阵与安全边界见
[`doc/adr/work-products/P02-provider-data-retirement.md`](../../doc/adr/work-products/P02-provider-data-retirement.md)。
