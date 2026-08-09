#!/usr/bin/env python3
"""Offline round-trip/rollback test for P2-W1 SQLite migration scripts."""

from __future__ import annotations

import json
import shutil
import sqlite3
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PREFLIGHT = HERE / "provider_retirement_preflight_v1.sql"
RETIRE = HERE / "retire_providers_v1.sql"
RESTORE = HERE / "restore_retired_providers_v1.sql"
RETIREMENT_ID = "retire-nexos-openai-v1"
TARGETS = ("nexosapi", "openai")


def run_script(db: Path, script: Path, *, should_pass: bool = True) -> subprocess.CompletedProcess[bytes]:
    sqlite = shutil.which("sqlite3")
    if sqlite is None:
        raise RuntimeError("sqlite3 CLI is required (scripts intentionally use .bail on)")
    result = subprocess.run(
        [sqlite, str(db)], input=script.read_bytes(), capture_output=True, check=False
    )
    if should_pass and result.returncode != 0:
        raise AssertionError(
            f"{script.name} failed ({result.returncode}):\n"
            f"{result.stderr.decode(errors='replace')}"
        )
    if not should_pass and result.returncode == 0:
        raise AssertionError(f"{script.name} unexpectedly succeeded")
    return result


def create_fixture(path: Path) -> None:
    with sqlite3.connect(path) as db:
        # Account/channel deliberately use the column order produced by an older
        # base table plus later ALTERs. The migration validates column sets, not
        # fragile cid order.
        db.executescript(
            """
            CREATE TABLE account (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                updatetime DATETIME DEFAULT CURRENT_TIMESTAMP,
                createtime DATETIME DEFAULT CURRENT_TIMESTAMP,
                apiname TEXT, username TEXT, password TEXT, authtoken TEXT,
                usecount INTEGER, tokenstatus INTEGER, accountstatus INTEGER,
                usertobitid INTEGER, personid TEXT, accounttype TEXT DEFAULT 'free',
                status TEXT DEFAULT 'active',
                workspaceuacid INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE channel (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                channelname TEXT UNIQUE NOT NULL, channeltype TEXT NOT NULL,
                channelurl TEXT, channelkey TEXT, channelstatus INTEGER DEFAULT 1,
                maxconcurrent INTEGER DEFAULT 10, timeout INTEGER DEFAULT 30,
                priority INTEGER DEFAULT 0, description TEXT,
                createtime DATETIME DEFAULT CURRENT_TIMESTAMP,
                updatetime DATETIME DEFAULT CURRENT_TIMESTAMP,
                accountcount INTEGER DEFAULT 0,
                supports_tool_calls INTEGER DEFAULT 1,
                accountretentiondays INTEGER DEFAULT 0
            );
            CREATE TABLE chat_session_state (
                session_id TEXT PRIMARY KEY, api_name TEXT DEFAULT '',
                api_type INTEGER DEFAULT 0, context_key TEXT DEFAULT '',
                payload TEXT NOT NULL, created_at INTEGER DEFAULT 0,
                last_active_at INTEGER DEFAULT 0
            );
            CREATE TABLE response_index (
                response_id TEXT PRIMARY KEY, session_id TEXT NOT NULL,
                response_body TEXT, has_response INTEGER DEFAULT 0,
                created_at INTEGER DEFAULT 0
            );
            CREATE TABLE error_event (id INTEGER PRIMARY KEY, provider TEXT);
            CREATE TABLE error_agg_hour (id INTEGER PRIMARY KEY, provider TEXT);
            CREATE TABLE request_agg_hour (id INTEGER PRIMARY KEY, provider TEXT);
            CREATE TABLE retool_workspace (
                id INTEGER PRIMARY KEY,
                openai_resource_uuid TEXT DEFAULT '',
                openai_resource_name TEXT DEFAULT ''
            );

            INSERT INTO account VALUES
              (7,'2026-01-02','2026-01-01','nexosapi','n-user','n-pass','n-token',3,1,1,11,'n-person','free','active',0),
              (8,NULL,'2026-02-01','openai','o-user','o-pass','o-token',NULL,0,1,NULL,NULL,'paid','waiting',42),
              (9,'2026-03-02','2026-03-01','chaynsapi','keep-user','keep-pass','keep-token',1,1,1,9,'keep-person','free','active',0);

            INSERT INTO channel VALUES
              (17,'nexosapi','nexosapi','https://retired.invalid','secret-key',1,4,31,2,'nexos','2026-01-01','2026-01-02',2,0,14),
              (18,'direct-openai-custom','openai',NULL,NULL,0,1,5,0,NULL,NULL,NULL,0,1,0),
              (19,'retoolapi','retool','/retoolapi/v1/chat/completions','',1,10,900,0,'keep','2026-01-01','2026-01-01',1,0,0);

            INSERT INTO chat_session_state VALUES
              ('s-nexos','nexosapi',0,'ctx-n','{"secret":"n"}',10,20),
              ('s-openai','openai',1,'ctx-o','{"secret":"o"}',30,40),
              ('s-keep','chaynsapi',1,'ctx-k','{"keep":true}',50,60);
            INSERT INTO response_index VALUES ('resp-n','s-nexos','{}',1,10);
            INSERT INTO error_event VALUES (1,'openai');
            INSERT INTO error_agg_hour VALUES (1,'nexosapi');
            INSERT INTO request_agg_hour VALUES (1,'openai');
            INSERT INTO retool_workspace VALUES (1,'resource-uuid','resource-name');
            """
        )


def rows(db: sqlite3.Connection, table: str) -> list[tuple]:
    pk = {"account": "id", "channel": "id", "chat_session_state": "session_id"}[table]
    return db.execute(f'SELECT * FROM "{table}" ORDER BY "{pk}"').fetchall()


def target_count(db: sqlite3.Connection) -> int:
    return db.execute(
        """
        SELECT
          (SELECT COUNT(*) FROM account WHERE apiname IN ('nexosapi','openai')) +
          (SELECT COUNT(*) FROM channel
             WHERE channelname IN ('nexosapi','openai')
                OR channeltype IN ('nexosapi','openai')) +
          (SELECT COUNT(*) FROM chat_session_state
             WHERE api_name IN ('nexosapi','openai'))
        """
    ).fetchone()[0]


def marker(db: sqlite3.Connection) -> tuple:
    value = db.execute(
        """SELECT status, expected_rows, archived_rows, deleted_rows
           FROM provider_retirement_migration_v1 WHERE retirement_id=?""",
        (RETIREMENT_ID,),
    ).fetchone()
    if value is None:
        raise AssertionError("migration marker missing")
    return value


def assert_snapshot_shape(db: sqlite3.Connection) -> None:
    expected = {
        "account": {
            "id", "updatetime", "createtime", "apiname", "username", "password",
            "authtoken", "usecount", "tokenstatus", "accountstatus", "usertobitid",
            "personid", "accounttype", "workspaceuacid", "status",
        },
        "channel": {
            "id", "channelname", "channeltype", "channelurl", "channelkey",
            "channelstatus", "maxconcurrent", "timeout", "priority", "description",
            "createtime", "updatetime", "accountcount", "accountretentiondays",
            "supports_tool_calls",
        },
        "chat_session_state": {
            "session_id", "api_name", "api_type", "context_key", "payload",
            "created_at", "last_active_at",
        },
    }
    for table, snapshot in db.execute(
        """SELECT source_table, row_snapshot FROM retired_provider_row_v1
           WHERE retirement_id=?""",
        (RETIREMENT_ID,),
    ):
        assert set(json.loads(snapshot)) == expected[table]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="aiapi-provider-retirement-") as tmp:
        path = Path(tmp) / "fixture.db"
        create_fixture(path)

        with sqlite3.connect(path) as db:
            before = {table: rows(db, table) for table in ("account", "channel", "chat_session_state")}
            retained_before = tuple(
                db.execute(
                    """SELECT
                      (SELECT COUNT(*) FROM response_index),
                      (SELECT COUNT(*) FROM error_event),
                      (SELECT COUNT(*) FROM error_agg_hour),
                      (SELECT COUNT(*) FROM request_agg_hour),
                      (SELECT COUNT(*) FROM retool_workspace
                       WHERE openai_resource_uuid <> '' OR openai_resource_name <> '')"""
                ).fetchone()
            )

        preflight = run_script(path, PREFLIGHT)
        output = preflight.stdout.decode(errors="replace")
        assert "retire.account" in output and "retain.error_event" in output
        for secret in ("n-pass", "n-token", "o-pass", "o-token", '"secret":"n"'):
            assert secret not in output, "preflight leaked a credential/payload"

        run_script(path, RETIRE)
        with sqlite3.connect(path) as db:
            assert target_count(db) == 0
            assert marker(db) == ("applied", 6, 6, 6)
            assert db.execute(
                "SELECT COUNT(*) FROM retired_provider_row_v1 WHERE retirement_id=?",
                (RETIREMENT_ID,),
            ).fetchone()[0] == 6
            assert_snapshot_shape(db)
            archive_before_rerun = db.execute(
                """SELECT source_table, original_pk, provider_key, row_snapshot, archived_at
                   FROM retired_provider_row_v1 WHERE retirement_id=?
                   ORDER BY source_table, original_pk""",
                (RETIREMENT_ID,),
            ).fetchall()
            marker_before_rerun = marker(db)
            retained_after = tuple(
                db.execute(
                    """SELECT
                      (SELECT COUNT(*) FROM response_index),
                      (SELECT COUNT(*) FROM error_event),
                      (SELECT COUNT(*) FROM error_agg_hour),
                      (SELECT COUNT(*) FROM request_agg_hour),
                      (SELECT COUNT(*) FROM retool_workspace
                       WHERE openai_resource_uuid <> '' OR openai_resource_name <> '')"""
                ).fetchone()
            )
            assert retained_after == retained_before

        # Applied rerun is an exact no-op for persistent archive/marker state.
        run_script(path, RETIRE)
        with sqlite3.connect(path) as db:
            assert marker(db) == marker_before_rerun
            assert db.execute(
                """SELECT source_table, original_pk, provider_key, row_snapshot, archived_at
                   FROM retired_provider_row_v1 WHERE retirement_id=?
                   ORDER BY source_table, original_pk""",
                (RETIREMENT_ID,),
            ).fetchall() == archive_before_rerun

        run_script(path, RESTORE)
        with sqlite3.connect(path) as db:
            assert marker(db)[0] == "restored"
            assert {table: rows(db, table) for table in before} == before
            restored_state = {table: rows(db, table) for table in before}

        # Restore is deliberately fail-fast, not overwrite-idempotent: every
        # original primary key is now a collision and the live DB stays intact.
        conflict = run_script(path, RESTORE, should_pass=False)
        assert b"restore_retired_providers_v1_guard" in conflict.stderr
        with sqlite3.connect(path) as db:
            assert marker(db)[0] == "restored"
            assert {table: rows(db, table) for table in before} == restored_state

            # Change a restored live row. Re-retirement must detect that the
            # stable archive snapshot no longer matches and roll back all deletes.
            db.execute("UPDATE account SET authtoken='changed-after-restore' WHERE id=7")
            db.commit()
        mismatch = run_script(path, RETIRE, should_pass=False)
        assert b"retire_providers_v1_guard" in mismatch.stderr
        with sqlite3.connect(path) as db:
            assert marker(db)[0] == "restored"
            assert target_count(db) == 6
            db.execute("UPDATE account SET authtoken='n-token' WHERE id=7")
            db.commit()

        # Exact restored rows may be re-retired into the same stable batch.
        run_script(path, RETIRE)
        with sqlite3.connect(path) as db:
            assert marker(db) == ("applied", 6, 6, 6)
            assert target_count(db) == 0
            # Primary-key conflict with an unrelated row aborts the whole restore.
            db.execute(
                """INSERT INTO account
                   (id,apiname,username,password,authtoken,usecount,tokenstatus,
                    accountstatus,usertobitid,personid,accounttype,workspaceuacid,status)
                   VALUES (7,'chaynsapi','collision','x','x',0,1,1,0,'','free',0,'active')"""
            )
            db.commit()
        pk_conflict = run_script(path, RESTORE, should_pass=False)
        assert b"restore_retired_providers_v1_guard" in pk_conflict.stderr
        with sqlite3.connect(path) as db:
            assert marker(db)[0] == "applied" and target_count(db) == 0
            db.execute("DELETE FROM account WHERE id=7")
            # Independent channelname UNIQUE conflict is also detected before insert.
            db.execute(
                """INSERT INTO channel (id,channelname,channeltype)
                   VALUES (999,'nexosapi','unrelated')"""
            )
            db.commit()
        unique_conflict = run_script(path, RESTORE, should_pass=False)
        assert b"restore_retired_providers_v1_guard" in unique_conflict.stderr
        with sqlite3.connect(path) as db:
            assert marker(db)[0] == "applied"
            assert db.execute("SELECT channeltype FROM channel WHERE id=999").fetchone() == ("unrelated",)
            # No other target table was partially restored.
            assert db.execute(
                "SELECT COUNT(*) FROM account WHERE apiname IN ('nexosapi','openai')"
            ).fetchone()[0] == 0
            assert db.execute(
                "SELECT COUNT(*) FROM chat_session_state WHERE api_name IN ('nexosapi','openai')"
            ).fetchone()[0] == 0

        # A future column makes the v1 JSON snapshot incomplete. The schema
        # guard must reject the migration before any persistent archive/marker
        # table or deletion is committed.
        drift_path = Path(tmp) / "schema-drift.db"
        create_fixture(drift_path)
        with sqlite3.connect(drift_path) as db:
            db.execute("ALTER TABLE account ADD COLUMN future_provider_field TEXT")
            db.commit()
        schema_drift = run_script(drift_path, RETIRE, should_pass=False)
        assert b"retire_providers_v1_guard" in schema_drift.stderr
        with sqlite3.connect(drift_path) as db:
            assert target_count(db) == 6
            assert db.execute(
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
                "('retired_provider_row_v1','provider_retirement_migration_v1')"
            ).fetchone()[0] == 0

    print("PASS provider retirement fixture: preflight, retire, idempotence, restore, rollback, conflicts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
