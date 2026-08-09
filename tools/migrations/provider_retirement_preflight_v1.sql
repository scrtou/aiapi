-- P2-W1 read-only preflight for the CURRENT SQLite schema.
-- Usage: sqlite3 /path/to/aiapi.db < tools/migrations/provider_retirement_preflight_v1.sql
-- This report never selects credentials or payload contents.
.bail on
.headers on
.mode column
PRAGMA query_only = ON;

SELECT 'database' AS section,
       sqlite_version() AS sqlite_version,
       (SELECT user_version FROM pragma_user_version) AS schema_user_version;

SELECT 'schema.column' AS section, m.name AS table_name,
       p.cid, p.name AS column_name, p.type, p."notnull", p.pk
FROM sqlite_master AS m
JOIN pragma_table_info(m.name) AS p
WHERE m.type = 'table'
  AND m.name IN ('account', 'channel', 'chat_session_state')
ORDER BY m.name, p.cid;

SELECT 'schema.foreign_key' AS section, 'account' AS table_name,
       id, seq, "table" AS target_table, "from" AS source_column, "to" AS target_column
FROM pragma_foreign_key_list('account')
UNION ALL
SELECT 'schema.foreign_key', 'channel', id, seq, "table", "from", "to"
FROM pragma_foreign_key_list('channel')
UNION ALL
SELECT 'schema.foreign_key', 'chat_session_state', id, seq, "table", "from", "to"
FROM pragma_foreign_key_list('chat_session_state');

WITH providers(provider_key) AS (VALUES ('nexosapi'), ('openai')),
channel_targets AS (
    SELECT id,
           CASE WHEN channelname IN ('nexosapi', 'openai')
                THEN channelname ELSE channeltype END AS provider_key
    FROM channel
    WHERE channelname IN ('nexosapi', 'openai')
       OR channeltype IN ('nexosapi', 'openai')
)
SELECT 'retire.account' AS section, p.provider_key,
       COUNT(a.id) AS row_count, MIN(a.id) AS min_pk, MAX(a.id) AS max_pk
FROM providers AS p
LEFT JOIN account AS a ON a.apiname = p.provider_key
GROUP BY p.provider_key
UNION ALL
SELECT 'retire.channel', p.provider_key,
       COUNT(ch.id), MIN(ch.id), MAX(ch.id)
FROM providers AS p
LEFT JOIN channel_targets AS ch ON ch.provider_key = p.provider_key
GROUP BY p.provider_key
UNION ALL
SELECT 'retire.chat_session_state', p.provider_key,
       COUNT(s.session_id), NULL, NULL
FROM providers AS p
LEFT JOIN chat_session_state AS s ON s.api_name = p.provider_key
GROUP BY p.provider_key;

-- Historical metrics are intentionally retained, but their provider dimensions
-- must be known before P2-W2 changes dashboards/alerts.
WITH providers(provider_key) AS (VALUES ('nexosapi'), ('openai'))
SELECT 'retain.error_event' AS section, p.provider_key, COUNT(e.id) AS row_count
FROM providers AS p
LEFT JOIN error_event AS e ON e.provider = p.provider_key
GROUP BY p.provider_key
UNION ALL
SELECT 'retain.error_agg_hour', p.provider_key, COUNT(e.rowid)
FROM providers AS p
LEFT JOIN error_agg_hour AS e ON e.provider = p.provider_key
GROUP BY p.provider_key
UNION ALL
SELECT 'retain.request_agg_hour', p.provider_key, COUNT(r.rowid)
FROM providers AS p
LEFT JOIN request_agg_hour AS r ON r.provider = p.provider_key
GROUP BY p.provider_key;

SELECT 'retain.response_index_for_retired_session' AS section, COUNT(*) AS row_count
FROM response_index AS r
JOIN chat_session_state AS s ON s.session_id = r.session_id
WHERE s.api_name IN ('nexosapi', 'openai');

-- These Retool fields are legal business data and MUST NOT be treated as
-- OpenAiProvider rows.
SELECT 'retain.retool_openai_resource_fields' AS section, COUNT(*) AS row_count
FROM retool_workspace
WHERE COALESCE(openai_resource_uuid, '') <> ''
   OR COALESCE(openai_resource_name, '') <> '';
