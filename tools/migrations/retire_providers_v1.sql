-- P2-W1 retirement migration for the CURRENT SQLite schema.
-- Provider keys: nexosapi, openai
-- Stable retirement id: retire-nexos-openai-v1
-- Usage (only after backup + preflight):
--   sqlite3 /path/to/aiapi.db < tools/migrations/retire_providers_v1.sql
-- The sqlite3 CLI's .bail is intentional: a failed guard closes the connection,
-- causing the open transaction to roll back before any later statement can run.
.bail on
PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;

CREATE TABLE IF NOT EXISTS retired_provider_row_v1 (
    retirement_id TEXT NOT NULL,
    source_table TEXT NOT NULL
        CHECK (source_table IN ('account', 'channel', 'chat_session_state')),
    original_pk TEXT NOT NULL,
    provider_key TEXT NOT NULL,
    row_snapshot TEXT NOT NULL CHECK (json_valid(row_snapshot)),
    archived_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (retirement_id, source_table, original_pk)
);

CREATE TABLE IF NOT EXISTS provider_retirement_migration_v1 (
    retirement_id TEXT PRIMARY KEY,
    migration_name TEXT NOT NULL UNIQUE,
    status TEXT NOT NULL CHECK (status IN ('applied', 'restored')),
    expected_rows INTEGER NOT NULL CHECK (expected_rows >= 0),
    archived_rows INTEGER NOT NULL CHECK (archived_rows = expected_rows),
    deleted_rows INTEGER NOT NULL CHECK (deleted_rows = expected_rows),
    applied_at TEXT NOT NULL,
    restored_at TEXT
);

DROP TABLE IF EXISTS temp._retire_v1_guard;
CREATE TEMP TABLE _retire_v1_guard (
    ok INTEGER NOT NULL CONSTRAINT retire_providers_v1_guard CHECK (ok = 1)
);

-- Refuse a future/older schema: otherwise the JSON would no longer be a
-- complete row snapshot. Upgraded databases can have the same columns in a
-- different cid order, so validate the exact column set rather than its order.
INSERT INTO _retire_v1_guard(ok)
SELECT (
    (SELECT COUNT(*) FROM pragma_table_info('account')) = 15
    AND (SELECT COUNT(*) FROM pragma_table_info('account') WHERE name IN
      ('id','updatetime','createtime','apiname','username','password','authtoken',
       'usecount','tokenstatus','accountstatus','usertobitid','personid',
       'accounttype','workspaceuacid','status')) = 15
    AND
    (SELECT COUNT(*) FROM pragma_table_info('channel')) = 15
    AND (SELECT COUNT(*) FROM pragma_table_info('channel') WHERE name IN
      ('id','channelname','channeltype','channelurl','channelkey','channelstatus',
       'maxconcurrent','timeout','priority','description','createtime','updatetime',
       'accountcount','accountretentiondays','supports_tool_calls')) = 15
    AND
    (SELECT COUNT(*) FROM pragma_table_info('chat_session_state')) = 7
    AND (SELECT COUNT(*) FROM pragma_table_info('chat_session_state') WHERE name IN
      ('session_id','api_name','api_type','context_key','payload','created_at',
       'last_active_at')) = 7
);
DELETE FROM _retire_v1_guard;

DROP TABLE IF EXISTS temp._retire_v1_context;
CREATE TEMP TABLE _retire_v1_context (
    retirement_id TEXT PRIMARY KEY,
    prior_status TEXT NOT NULL,
    expected_rows INTEGER NOT NULL,
    deleted_rows INTEGER NOT NULL DEFAULT 0
);
INSERT INTO _retire_v1_context(retirement_id, prior_status, expected_rows)
SELECT
    'retire-nexos-openai-v1',
    COALESCE((
        SELECT status FROM provider_retirement_migration_v1
        WHERE retirement_id = 'retire-nexos-openai-v1'
    ), 'new'),
    (SELECT COUNT(*) FROM account
       WHERE apiname IN ('nexosapi', 'openai'))
    + (SELECT COUNT(*) FROM channel
       WHERE channelname IN ('nexosapi', 'openai')
          OR channeltype IN ('nexosapi', 'openai'))
    + (SELECT COUNT(*) FROM chat_session_state
       WHERE api_name IN ('nexosapi', 'openai'));

-- An already-applied migration is idempotent only while no new retired-provider
-- live rows have reappeared. Drift must stop rather than be silently absorbed.
INSERT INTO _retire_v1_guard(ok)
SELECT CASE
    WHEN prior_status = 'applied' AND expected_rows = 0 THEN 1
    WHEN prior_status IN ('new', 'restored') THEN 1
    ELSE 0
END
FROM _retire_v1_context;
DELETE FROM _retire_v1_guard;

INSERT INTO retired_provider_row_v1
    (retirement_id, source_table, original_pk, provider_key, row_snapshot)
SELECT 'retire-nexos-openai-v1', 'account', CAST(id AS TEXT), apiname,
       json_object(
           'id', id, 'updatetime', updatetime, 'createtime', createtime,
           'apiname', apiname, 'username', username, 'password', password,
           'authtoken', authtoken, 'usecount', usecount,
           'tokenstatus', tokenstatus, 'accountstatus', accountstatus,
           'usertobitid', usertobitid, 'personid', personid,
           'accounttype', accounttype, 'workspaceuacid', workspaceuacid,
           'status', status)
FROM account
WHERE apiname IN ('nexosapi', 'openai')
  AND (SELECT prior_status <> 'applied' FROM _retire_v1_context)
ON CONFLICT(retirement_id, source_table, original_pk) DO NOTHING;

INSERT INTO retired_provider_row_v1
    (retirement_id, source_table, original_pk, provider_key, row_snapshot)
SELECT 'retire-nexos-openai-v1', 'channel', CAST(id AS TEXT),
       CASE WHEN channelname IN ('nexosapi', 'openai') THEN channelname ELSE channeltype END,
       json_object(
           'id', id, 'channelname', channelname, 'channeltype', channeltype,
           'channelurl', channelurl, 'channelkey', channelkey,
           'channelstatus', channelstatus, 'maxconcurrent', maxconcurrent,
           'timeout', timeout, 'priority', priority, 'description', description,
           'createtime', createtime, 'updatetime', updatetime,
           'accountcount', accountcount,
           'accountretentiondays', accountretentiondays,
           'supports_tool_calls', supports_tool_calls)
FROM channel
WHERE (channelname IN ('nexosapi', 'openai')
    OR channeltype IN ('nexosapi', 'openai'))
  AND (SELECT prior_status <> 'applied' FROM _retire_v1_context)
ON CONFLICT(retirement_id, source_table, original_pk) DO NOTHING;

INSERT INTO retired_provider_row_v1
    (retirement_id, source_table, original_pk, provider_key, row_snapshot)
SELECT 'retire-nexos-openai-v1', 'chat_session_state', session_id, api_name,
       json_object(
           'session_id', session_id, 'api_name', api_name,
           'api_type', api_type, 'context_key', context_key,
           'payload', payload, 'created_at', created_at,
           'last_active_at', last_active_at)
FROM chat_session_state
WHERE api_name IN ('nexosapi', 'openai')
  AND (SELECT prior_status <> 'applied' FROM _retire_v1_context)
ON CONFLICT(retirement_id, source_table, original_pk) DO NOTHING;

-- Exact archive reconciliation: every live candidate must have the same primary
-- key AND byte-equivalent canonical JSON snapshot, and there may be no stale
-- extra row in this stable retirement batch.
INSERT INTO _retire_v1_guard(ok)
SELECT CASE
  WHEN c.prior_status = 'applied' THEN
    CASE WHEN c.expected_rows = 0
           AND (SELECT COUNT(*) FROM retired_provider_row_v1
                WHERE retirement_id = c.retirement_id) =
               (SELECT archived_rows FROM provider_retirement_migration_v1
                WHERE retirement_id = c.retirement_id AND status = 'applied')
         THEN 1 ELSE 0 END
  ELSE
    CASE WHEN
      (SELECT COUNT(*)
       FROM account AS a
       JOIN retired_provider_row_v1 AS r
         ON r.retirement_id = c.retirement_id
        AND r.source_table = 'account'
        AND r.original_pk = CAST(a.id AS TEXT)
        AND r.row_snapshot = json_object(
           'id', a.id, 'updatetime', a.updatetime, 'createtime', a.createtime,
           'apiname', a.apiname, 'username', a.username, 'password', a.password,
           'authtoken', a.authtoken, 'usecount', a.usecount,
           'tokenstatus', a.tokenstatus, 'accountstatus', a.accountstatus,
           'usertobitid', a.usertobitid, 'personid', a.personid,
           'accounttype', a.accounttype, 'workspaceuacid', a.workspaceuacid,
           'status', a.status)
       WHERE a.apiname IN ('nexosapi', 'openai'))
      +
      (SELECT COUNT(*)
       FROM channel AS ch
       JOIN retired_provider_row_v1 AS r
         ON r.retirement_id = c.retirement_id
        AND r.source_table = 'channel'
        AND r.original_pk = CAST(ch.id AS TEXT)
        AND r.row_snapshot = json_object(
           'id', ch.id, 'channelname', ch.channelname,
           'channeltype', ch.channeltype, 'channelurl', ch.channelurl,
           'channelkey', ch.channelkey, 'channelstatus', ch.channelstatus,
           'maxconcurrent', ch.maxconcurrent, 'timeout', ch.timeout,
           'priority', ch.priority, 'description', ch.description,
           'createtime', ch.createtime, 'updatetime', ch.updatetime,
           'accountcount', ch.accountcount,
           'accountretentiondays', ch.accountretentiondays,
           'supports_tool_calls', ch.supports_tool_calls)
       WHERE ch.channelname IN ('nexosapi', 'openai')
          OR ch.channeltype IN ('nexosapi', 'openai'))
      +
      (SELECT COUNT(*)
       FROM chat_session_state AS s
       JOIN retired_provider_row_v1 AS r
         ON r.retirement_id = c.retirement_id
        AND r.source_table = 'chat_session_state'
        AND r.original_pk = s.session_id
        AND r.row_snapshot = json_object(
           'session_id', s.session_id, 'api_name', s.api_name,
           'api_type', s.api_type, 'context_key', s.context_key,
           'payload', s.payload, 'created_at', s.created_at,
           'last_active_at', s.last_active_at)
       WHERE s.api_name IN ('nexosapi', 'openai'))
      = c.expected_rows
      AND (SELECT COUNT(*) FROM retired_provider_row_v1
           WHERE retirement_id = c.retirement_id) = c.expected_rows
    THEN 1 ELSE 0 END
END
FROM _retire_v1_context AS c;
DELETE FROM _retire_v1_guard;

DELETE FROM account
WHERE apiname IN ('nexosapi', 'openai')
  AND (SELECT prior_status <> 'applied' FROM _retire_v1_context);
UPDATE _retire_v1_context SET deleted_rows = deleted_rows + changes();

DELETE FROM channel
WHERE (channelname IN ('nexosapi', 'openai')
    OR channeltype IN ('nexosapi', 'openai'))
  AND (SELECT prior_status <> 'applied' FROM _retire_v1_context);
UPDATE _retire_v1_context SET deleted_rows = deleted_rows + changes();

DELETE FROM chat_session_state
WHERE api_name IN ('nexosapi', 'openai')
  AND (SELECT prior_status <> 'applied' FROM _retire_v1_context);
UPDATE _retire_v1_context SET deleted_rows = deleted_rows + changes();

INSERT INTO _retire_v1_guard(ok)
SELECT CASE WHEN
    deleted_rows = CASE WHEN prior_status = 'applied' THEN 0 ELSE expected_rows END
    AND NOT EXISTS (SELECT 1 FROM account
                    WHERE apiname IN ('nexosapi', 'openai'))
    AND NOT EXISTS (SELECT 1 FROM channel
                    WHERE channelname IN ('nexosapi', 'openai')
                       OR channeltype IN ('nexosapi', 'openai'))
    AND NOT EXISTS (SELECT 1 FROM chat_session_state
                    WHERE api_name IN ('nexosapi', 'openai'))
  THEN 1 ELSE 0 END
FROM _retire_v1_context;
DELETE FROM _retire_v1_guard;

INSERT INTO provider_retirement_migration_v1
    (retirement_id, migration_name, status, expected_rows, archived_rows,
     deleted_rows, applied_at, restored_at)
SELECT retirement_id, 'retire_providers_v1', 'applied', expected_rows,
       expected_rows, deleted_rows, CURRENT_TIMESTAMP, NULL
FROM _retire_v1_context
WHERE prior_status = 'new';

UPDATE provider_retirement_migration_v1
SET status = 'applied',
    expected_rows = (SELECT expected_rows FROM _retire_v1_context),
    archived_rows = (SELECT expected_rows FROM _retire_v1_context),
    deleted_rows = (SELECT deleted_rows FROM _retire_v1_context),
    applied_at = CURRENT_TIMESTAMP
WHERE retirement_id = 'retire-nexos-openai-v1'
  AND (SELECT prior_status = 'restored' FROM _retire_v1_context);

INSERT INTO _retire_v1_guard(ok)
SELECT CASE WHEN m.status = 'applied'
    AND m.archived_rows = (SELECT COUNT(*) FROM retired_provider_row_v1 AS r
                           WHERE r.retirement_id = m.retirement_id)
    AND m.expected_rows = m.archived_rows
    AND m.deleted_rows = m.expected_rows
  THEN 1 ELSE 0 END
FROM provider_retirement_migration_v1 AS m
WHERE m.retirement_id = 'retire-nexos-openai-v1';

COMMIT;
