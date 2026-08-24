from __future__ import annotations

import sqlite3
from datetime import datetime
from pathlib import Path


class StateStore:
    def __init__(self, path: Path) -> None:
        self.path = path

    def init(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as conn:
            conn.execute(
                """
                CREATE TABLE IF NOT EXISTS file_state (
                  id INTEGER PRIMARY KEY AUTOINCREMENT,
                  device_id INTEGER NOT NULL,
                  local_path TEXT NOT NULL,
                  file_name TEXT NOT NULL,
                  file_size INTEGER NOT NULL,
                  file_mtime TEXT NOT NULL,
                  file_hash TEXT NOT NULL,
                  data_no TEXT,
                  status TEXT NOT NULL,
                  retry_count INTEGER NOT NULL DEFAULT 0,
                  last_error_code TEXT,
                  last_error_message TEXT,
                  first_seen_time TEXT NOT NULL,
                  last_seen_time TEXT NOT NULL,
                  uploaded_time TEXT,
                  finished_time TEXT,
                  created_time TEXT NOT NULL,
                  updated_time TEXT NOT NULL,
                  UNIQUE(device_id, local_path, file_size, file_mtime)
                )
                """
            )
            self._migrate(conn)

    def _migrate(self, conn: sqlite3.Connection) -> None:
        columns = {row["name"] for row in conn.execute("PRAGMA table_info(file_state)").fetchall()}
        migrations = {
            "retry_count": "ALTER TABLE file_state ADD COLUMN retry_count INTEGER NOT NULL DEFAULT 0",
            "first_seen_time": "ALTER TABLE file_state ADD COLUMN first_seen_time TEXT",
            "last_seen_time": "ALTER TABLE file_state ADD COLUMN last_seen_time TEXT",
            "uploaded_time": "ALTER TABLE file_state ADD COLUMN uploaded_time TEXT",
            "finished_time": "ALTER TABLE file_state ADD COLUMN finished_time TEXT",
        }
        for column, statement in migrations.items():
            if column not in columns:
                conn.execute(statement)
        now = datetime.now().isoformat()
        conn.execute("UPDATE file_state SET first_seen_time = COALESCE(first_seen_time, created_time, ?)", (now,))
        conn.execute("UPDATE file_state SET last_seen_time = COALESCE(last_seen_time, updated_time, ?)", (now,))

    def connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.path)
        conn.row_factory = sqlite3.Row
        return conn

    def get_by_file(self, device_id: int, local_path: str, file_size: int, file_mtime: str) -> sqlite3.Row | None:
        with self.connect() as conn:
            return conn.execute(
                """
                SELECT * FROM file_state
                WHERE device_id = ? AND local_path = ? AND file_size = ? AND file_mtime = ?
                """,
                (device_id, local_path, file_size, file_mtime),
            ).fetchone()

    def mark_uploading(
        self,
        *,
        device_id: int,
        local_path: str,
        file_name: str,
        file_size: int,
        file_mtime: str,
        file_hash: str,
    ) -> None:
        now = datetime.now().isoformat()
        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO file_state (
                  device_id, local_path, file_name, file_size, file_mtime, file_hash,
                  status, retry_count, first_seen_time, last_seen_time, created_time, updated_time
                ) VALUES (?, ?, ?, ?, ?, ?, 'uploading', 0, ?, ?, ?, ?)
                ON CONFLICT(device_id, local_path, file_size, file_mtime)
                DO UPDATE SET
                  status='uploading',
                  last_seen_time=excluded.last_seen_time,
                  last_error_code=NULL,
                  last_error_message=NULL,
                  updated_time=excluded.updated_time
                """,
                (device_id, local_path, file_name, file_size, file_mtime, file_hash, now, now, now, now),
            )

    def mark_uploaded(self, *, device_id: int, local_path: str, file_size: int, file_mtime: str, data_no: str) -> None:
        now = datetime.now().isoformat()
        with self.connect() as conn:
            conn.execute(
                """
                UPDATE file_state
                SET status='uploaded',
                    data_no=?,
                    uploaded_time=?,
                    last_error_code=NULL,
                    last_error_message=NULL,
                    updated_time=?
                WHERE device_id=? AND local_path=? AND file_size=? AND file_mtime=?
                """,
                (data_no, now, now, device_id, local_path, file_size, file_mtime),
            )

    def mark_upload_failed(
        self,
        *,
        device_id: int,
        local_path: str,
        file_size: int,
        file_mtime: str,
        error_message: str,
    ) -> None:
        now = datetime.now().isoformat()
        with self.connect() as conn:
            conn.execute(
                """
                UPDATE file_state
                SET status='upload_failed',
                    retry_count=retry_count + 1,
                    last_error_code='UPLOAD_FAILED',
                    last_error_message=?,
                    updated_time=?
                WHERE device_id=? AND local_path=? AND file_size=? AND file_mtime=?
                """,
                (error_message[:2000], now, device_id, local_path, file_size, file_mtime),
            )

    def apply_task_result(self, data: dict) -> None:
        status = "parse_success" if data.get("status") == "success" else "parse_failed"
        now = datetime.now().isoformat()
        with self.connect() as conn:
            conn.execute(
                """
                UPDATE file_state
                SET status=?,
                    last_error_code=?,
                    last_error_message=?,
                    finished_time=?,
                    updated_time=?
                WHERE data_no=?
                """,
                (
                    status,
                    data.get("errorCode"),
                    data.get("errorMessage"),
                    now,
                    now,
                    data.get("dataNo"),
                ),
            )

    def apply_statuses(self, statuses: list[dict]) -> int:
        updated_count = 0
        for item in statuses:
            status = item.get("status")
            if status not in {"success", "failed"}:
                continue
            self.apply_task_result(item)
            updated_count += 1
        return updated_count

    def uploaded_data_nos(self) -> list[str]:
        with self.connect() as conn:
            rows = conn.execute(
                """
                SELECT data_no FROM file_state
                WHERE data_no IS NOT NULL AND status IN ('uploaded', 'uploading')
                ORDER BY id
                """
            ).fetchall()
        return [row["data_no"] for row in rows]

    def list_records(self, *, status: str | None = None, limit: int = 100) -> list[dict]:
        sql = """
            SELECT * FROM file_state
        """
        params: list[str | int] = []
        if status:
            sql += " WHERE status = ?"
            params.append(status)
        sql += " ORDER BY updated_time DESC, id DESC LIMIT ?"
        params.append(limit)
        with self.connect() as conn:
            rows = conn.execute(sql, params).fetchall()
        return [dict(row) for row in rows]

    def failed_uploads(self, *, limit: int = 100) -> list[dict]:
        return self.list_records(status="upload_failed", limit=limit)

    def clear_failed_records(self) -> int:
        with self.connect() as conn:
            cursor = conn.execute(
                """
                DELETE FROM file_state
                WHERE status IN ('upload_failed', 'parse_failed')
                """
            )
            return cursor.rowcount

    def clear_records(self) -> int:
        with self.connect() as conn:
            cursor = conn.execute("DELETE FROM file_state")
            return cursor.rowcount

    def recover_uploading(self) -> int:
        now = datetime.now().isoformat()
        with self.connect() as conn:
            cursor = conn.execute(
                """
                UPDATE file_state
                SET status='upload_failed',
                    last_error_code='UPLOAD_INTERRUPTED',
                    last_error_message='upload interrupted before workstation restart',
                    updated_time=?
                WHERE status='uploading'
                """,
                (now,),
            )
            return cursor.rowcount
