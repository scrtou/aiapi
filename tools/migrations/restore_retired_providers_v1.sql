-- P2-W1 restore migration for the CURRENT SQLite schema.
-- Restores the stable batch retire-nexos-openai-v1.
-- Any source primary-key conflict, or channelname unique-key conflict, aborts
-- the entire transaction. Existing rows are never overwritten.
-- Usage: sqlite3 /path/to/aiapi.db < tools/migrations/restore_retired_providers_v1.sql
.bail on
PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;

-- Define the expected archive contract so a missing retirement migration fails
-- through a named guard rather than producing a partial restore.
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

DROP TABLE IF EXISTS temp._restore_v1_guard;
CREATE TEMP TABLE _restore_v1_guard (
    ok INTEGER NOT NULL CONSTRAINT restore_retired_providers_v1_guard CHECK (ok = 1)
);

INSERT INTO _restore_v1_guard(ok)
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
DELETE FROM _restore_v1_guard;

-- Marker and archive reconciliation must pass before touching live tables.
INSERT INTO _restore_v1_guard(ok)
SELECT CASE WHEN COUNT(*) = 1
    AND MAX(status) = 'applied'
    AND MAX(archived_rows) = (
        SELECT COUNT(*) FROM retired_provider_row_v1
        WHERE retirement_id = 'retire-nexos-openai-v1')
  THEN 1 ELSE 0 END
FROM provider_retirement_migration_v1
WHERE retirement_id = 'retire-nexos-openai-v1'
  AND migration_name = 'retire_providers_v1';
DELETE FROM _restore_v1_guard;

-- Detect both primary keys and the channel table's independent UNIQUE key.
INSERT INTO _restore_v1_guard(ok)
SELECT CASE WHEN
  NOT EXISTS (
    SELECT 1 FROM retired_provider_row_v1 AS r
    JOIN account AS a ON CAST(a.id AS TEXT) = r.original_pk
    WHERE r.retirement_id = 'retire-nexos-openai-v1'
      AND r.source_table = 'account')
  AND NOT EXISTS (
    SELECT 1 FROM retired_provider_row_v1 AS r
    JOIN channel AS ch
      ON CAST(ch.id AS TEXT) = r.original_pk
      OR ch.channelname = json_extract(r.row_snapshot, '$.channelname')
    WHERE r.retirement_id = 'retire-nexos-openai-v1'
      AND r.source_table = 'channel')
  AND NOT EXISTS (
    SELECT 1 FROM retired_provider_row_v1 AS r
    JOIN chat_session_state AS s ON s.session_id = r.original_pk
    WHERE r.retirement_id = 'retire-nexos-openai-v1'
      AND r.source_table = 'chat_session_state')
THEN 1 ELSE 0 END;
DELETE FROM _restore_v1_guard;

INSERT INTO account
    (id, updatetime, createtime, apiname, username, password, authtoken,
     usecount, tokenstatus, accountstatus, usertobitid, personid, accounttype,
     workspaceuacid, status)
SELECT
    CAST(json_extract(row_snapshot, '$.id') AS INTEGER),
    json_extract(row_snapshot, '$.updatetime'),
    json_extract(row_snapshot, '$.createtime'),
    json_extract(row_snapshot, '$.apiname'),
    json_extract(row_snapshot, '$.username'),
    json_extract(row_snapshot, '$.password'),
    json_extract(row_snapshot, '$.authtoken'),
    json_extract(row_snapshot, '$.usecount'),
    json_extract(row_snapshot, '$.tokenstatus'),
    json_extract(row_snapshot, '$.accountstatus'),
    json_extract(row_snapshot, '$.usertobitid'),
    json_extract(row_snapshot, '$.personid'),
    json_extract(row_snapshot, '$.accounttype'),
    json_extract(row_snapshot, '$.workspaceuacid'),
    json_extract(row_snapshot, '$.status')
FROM retired_provider_row_v1
WHERE retirement_id = 'retire-nexos-openai-v1'
  AND source_table = 'account';

INSERT INTO channel
    (id, channelname, channeltype, channelurl, channelkey, channelstatus,
     maxconcurrent, timeout, priority, description, createtime, updatetime,
     accountcount, accountretentiondays, supports_tool_calls)
SELECT
    CAST(json_extract(row_snapshot, '$.id') AS INTEGER),
    json_extract(row_snapshot, '$.channelname'),
    json_extract(row_snapshot, '$.channeltype'),
    json_extract(row_snapshot, '$.channelurl'),
    json_extract(row_snapshot, '$.channelkey'),
    json_extract(row_snapshot, '$.channelstatus'),
    json_extract(row_snapshot, '$.maxconcurrent'),
    json_extract(row_snapshot, '$.timeout'),
    json_extract(row_snapshot, '$.priority'),
    json_extract(row_snapshot, '$.description'),
    json_extract(row_snapshot, '$.createtime'),
    json_extract(row_snapshot, '$.updatetime'),
    json_extract(row_snapshot, '$.accountcount'),
    json_extract(row_snapshot, '$.accountretentiondays'),
    json_extract(row_snapshot, '$.supports_tool_calls')
FROM retired_provider_row_v1
WHERE retirement_id = 'retire-nexos-openai-v1'
  AND source_table = 'channel';

INSERT INTO chat_session_state
    (session_id, api_name, api_type, context_key, payload, created_at, last_active_at)
SELECT
    json_extract(row_snapshot, '$.session_id'),
    json_extract(row_snapshot, '$.api_name'),
    json_extract(row_snapshot, '$.api_type'),
    json_extract(row_snapshot, '$.context_key'),
    json_extract(row_snapshot, '$.payload'),
    json_extract(row_snapshot, '$.created_at'),
    json_extract(row_snapshot, '$.last_active_at')
FROM retired_provider_row_v1
WHERE retirement_id = 'retire-nexos-openai-v1'
  AND source_table = 'chat_session_state';

-- Round-trip reconciliation uses the same canonical JSON field order as archive.
INSERT INTO _restore_v1_guard(ok)
SELECT CASE WHEN
  (SELECT COUNT(*)
   FROM account AS a
   JOIN retired_provider_row_v1 AS r
     ON r.retirement_id = 'retire-nexos-openai-v1'
    AND r.source_table = 'account'
    AND r.original_pk = CAST(a.id AS TEXT)
    AND r.row_snapshot = json_object(
       'id', a.id, 'updatetime', a.updatetime, 'createtime', a.createtime,
       'apiname', a.apiname, 'username', a.username, 'password', a.password,
       'authtoken', a.authtoken, 'usecount', a.usecount,
       'tokenstatus', a.tokenstatus, 'accountstatus', a.accountstatus,
       'usertobitid', a.usertobitid, 'personid', a.personid,
       'accounttype', a.accounttype, 'workspaceuacid', a.workspaceuacid,
       'status', a.status))
  +
  (SELECT COUNT(*)
   FROM channel AS ch
   JOIN retired_provider_row_v1 AS r
     ON r.retirement_id = 'retire-nexos-openai-v1'
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
       'supports_tool_calls', ch.supports_tool_calls))
  +
  (SELECT COUNT(*)
   FROM chat_session_state AS s
   JOIN retired_provider_row_v1 AS r
     ON r.retirement_id = 'retire-nexos-openai-v1'
    AND r.source_table = 'chat_session_state'
    AND r.original_pk = s.session_id
    AND r.row_snapshot = json_object(
       'session_id', s.session_id, 'api_name', s.api_name,
       'api_type', s.api_type, 'context_key', s.context_key,
       'payload', s.payload, 'created_at', s.created_at,
       'last_active_at', s.last_active_at))
  = (SELECT archived_rows FROM provider_retirement_migration_v1
     WHERE retirement_id = 'retire-nexos-openai-v1')
THEN 1 ELSE 0 END;
DELETE FROM _restore_v1_guard;

UPDATE provider_retirement_migration_v1
SET status = 'restored', restored_at = CURRENT_TIMESTAMP
WHERE retirement_id = 'retire-nexos-openai-v1'
  AND status = 'applied';

INSERT INTO _restore_v1_guard(ok)
SELECT CASE WHEN changes() = 1
  AND (SELECT status FROM provider_retirement_migration_v1
       WHERE retirement_id = 'retire-nexos-openai-v1') = 'restored'
THEN 1 ELSE 0 END;

COMMIT;
