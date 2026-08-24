import asyncio
import json
import logging
from pathlib import Path
from datetime import datetime
import sqlite3

from workstation.client import ApiClient, multipart_body, sha256_file
from workstation.build_info import runtime_summary
from workstation.cli import apply_remote_config, configure_logging, query_status, upload
from workstation.config import ConfigError, WorkstationConfig, ensure_config, load_config, save_config, update_base_config
from workstation.runtime import FailedUploadRetrier
from workstation.scanner import DirectoryScanner
from workstation.state import StateStore
from workstation.ws_client import WebSocketClient
from workstation.windows_integration import doctor, startup_command


def test_workstation_config_round_trip(tmp_path: Path) -> None:
    path = tmp_path / "server.json"
    config = WorkstationConfig(
        api_base_url="http://127.0.0.1:8080",
        mac="00:11:22:33:44:55",
        workstation_token="token",
        ws_url="/file",
        items=[{"deviceId": 1, "watchPath": str(tmp_path), "fileType": "csv", "stableSeconds": 10}],
    )

    save_config(path, config)
    loaded = load_config(path)

    assert loaded.api_base_url == "http://127.0.0.1:8080"
    assert loaded.mac == "00:11:22:33:44:55"
    assert loaded.resolved_ws_url() == "ws://127.0.0.1:8080/file"
    assert loaded.items == [{"deviceId": 1, "watchPath": str(tmp_path), "fileType": "csv", "stableSeconds": 10}]


def test_gui_module_imports_without_pyside6() -> None:
    import workstation.gui as gui

    assert callable(gui.run_gui)


def test_gui_recent_status_display_mapping() -> None:
    import workstation.gui as gui

    window = gui.WorkstationMainWindow.__new__(gui.WorkstationMainWindow)

    assert window._display_recent_status("uploading") == "上传中"
    assert window._display_recent_status("uploaded") == "解析中"
    assert window._display_recent_status("parse_success") == "成功"
    assert window._display_recent_status("parse_failed") == "失败"


def test_gui_internal_tools_password_constant() -> None:
    import workstation.gui as gui

    assert gui.INTERNAL_TOOLS_PASSWORD == "199922"


def test_gui_notifies_parse_result_without_tray_icon() -> None:
    import workstation.gui as gui

    notices = []
    window = gui.WorkstationMainWindow.__new__(gui.WorkstationMainWindow)
    window.tray_icon = None
    window._notified_results = set()
    window.qt = {"TRAY_INFO": "info", "TRAY_WARNING": "warning"}
    window._show_parse_notice = lambda title, message, *, is_error: notices.append((title, message, is_error))

    window._notify_new_results(
        [{"status": "parse_success", "data_no": "DATA-001", "file_name": "result.docx"}]
    )

    assert notices == [("result.docx解析完成", "result.docx已完成解析", False)]


def test_gui_parse_notifications_are_deduplicated_with_tray_icon() -> None:
    import workstation.gui as gui

    class FakeTray:
        def __init__(self) -> None:
            self.messages = []

        def showMessage(self, title, message, icon, timeout) -> None:
            self.messages.append((title, message, icon, timeout))

    notices = []
    tray = FakeTray()
    window = gui.WorkstationMainWindow.__new__(gui.WorkstationMainWindow)
    window.tray_icon = tray
    window._notified_results = set()
    window.qt = {"TRAY_INFO": "info", "TRAY_WARNING": "warning"}
    window._show_parse_notice = lambda title, message, *, is_error: notices.append((title, message, is_error))

    rows = [{"status": "parse_failed", "data_no": "DATA-002", "file_name": "bad.docx"}]
    window._notify_new_results(rows)
    window._notify_new_results(rows)

    assert tray.messages == [("bad.docx解析失败", "bad.docx解析失败，请检查结果", "warning", 5000)]
    assert notices == [("bad.docx解析失败", "bad.docx解析失败，请检查结果", True)]


def test_gui_runtime_program_path_keeps_virtualenv_symlink(monkeypatch, tmp_path: Path) -> None:
    import workstation.gui as gui

    real_python = tmp_path / "uv-python" / "bin" / "python3.11"
    real_python.parent.mkdir(parents=True)
    real_python.write_text("# python", encoding="utf-8")
    venv_python = tmp_path / ".venv" / "bin" / "python3"
    venv_python.parent.mkdir(parents=True)
    venv_python.symlink_to(real_python)

    monkeypatch.setattr("sys.executable", str(venv_python))
    monkeypatch.setattr("sys.frozen", False, raising=False)

    assert gui._runtime_program_path() == venv_python


def test_gui_runtime_program_path_resolves_frozen_executable(monkeypatch, tmp_path: Path) -> None:
    import workstation.gui as gui

    real_executable = tmp_path / "NetStarFileParseWorkstation"
    real_executable.write_text("# app", encoding="utf-8")
    symlink_executable = tmp_path / "app-link"
    symlink_executable.symlink_to(real_executable)

    monkeypatch.setattr("sys.executable", str(symlink_executable))
    monkeypatch.setattr("sys.frozen", True, raising=False)

    assert gui._runtime_program_path() == real_executable


def test_runtime_summary_contains_build_identity() -> None:
    summary = runtime_summary("run")

    assert "target=" in summary
    assert "commit=" in summary
    assert "command=run" in summary
    assert "python=" in summary


def test_qt_compat_exposes_core_application_when_available() -> None:
    import workstation.qt_compat as qt_compat

    assert callable(qt_compat.load_qt)


def test_ensure_config_creates_default_file(tmp_path: Path) -> None:
    path = tmp_path / "server.json"

    config = ensure_config(path)

    assert path.exists()
    assert config.api_base_url == "http://127.0.0.1:8080"
    assert config.mac


def test_workstation_config_validation_rejects_invalid_values() -> None:
    config = WorkstationConfig(
        api_base_url="localhost:8080",
        mac="replace-with-workstation-mac",
        heartbeat_interval_seconds=0,
        items=[{"watchPath": "/tmp"}],
    )

    try:
        config.validate()
    except ConfigError as exc:
        message = str(exc)
    else:
        raise AssertionError("ConfigError was not raised")

    assert "apiBaseUrl" in message
    assert "mac must be configured" in message
    assert "heartbeatIntervalSeconds" in message
    assert "items[0].deviceId" in message


def test_update_base_config_preserves_remote_items(tmp_path: Path) -> None:
    path = tmp_path / "server.json"
    save_config(
        path,
        WorkstationConfig(
            api_base_url="http://old.example.com",
            mac="00:11:22:33:44:55",
            workstation_token="old-token",
            config_version=7,
            items=[{"deviceId": 1, "watchPath": str(tmp_path), "fileType": "csv", "stableSeconds": 10}],
        ),
    )

    updated = update_base_config(
        path,
        api_base_url="http://new.example.com",
        mac="00:11:22:33:44:66",
        workstation_token="new-token",
        ws_url="/file",
        heartbeat_interval_seconds=20,
    )

    loaded = load_config(path)
    assert updated.api_base_url == "http://new.example.com"
    assert loaded.mac == "00:11:22:33:44:66"
    assert loaded.config_version == 7
    assert loaded.items == [{"deviceId": 1, "watchPath": str(tmp_path), "fileType": "csv", "stableSeconds": 10}]


def test_apply_remote_config_updates_shared_runtime_config(tmp_path: Path) -> None:
    config_path = tmp_path / "server.json"
    config = WorkstationConfig(
        api_base_url="http://127.0.0.1:8080",
        mac="00:11:22:33:44:55",
        workstation_token="token",
        config_version=1,
    )
    save_config(config_path, config)

    changed = apply_remote_config(
        config,
        {
            "configVersion": 2,
            "items": [
                {
                    "deviceId": 1,
                    "watchPath": str(tmp_path),
                    "fileType": "excel",
                    "stableSeconds": 2,
                    "enabled": True,
                }
            ],
        },
        config_path,
    )

    assert changed is True
    assert config.config_version == 2
    assert config.items[0]["fileType"] == "excel"
    assert load_config(config_path).items == config.items


def test_configure_logging_writes_to_file(tmp_path: Path) -> None:
    log_file = tmp_path / "logs" / "workstation.log"

    configure_logging("INFO", log_file)
    __import__("logging").getLogger("workstation.test").info("hello workstation")

    assert "hello workstation" in log_file.read_text(encoding="utf-8")


def test_configure_logging_writes_without_stderr(tmp_path: Path, monkeypatch) -> None:
    log_file = tmp_path / "logs" / "workstation.log"
    monkeypatch.setattr("sys.stderr", None)

    configure_logging("INFO", log_file)
    logging.getLogger("workstation.win7").info("win7 file logging")

    assert "win7 file logging" in log_file.read_text(encoding="utf-8")


def test_startup_command_quotes_paths(tmp_path: Path) -> None:
    command = startup_command(
        "C:\\Program Files\\NetStar\\NetStarFileParseWorkstation.exe",
        config_path=tmp_path / "server.json",
        state_db=tmp_path / "state.db",
        log_file=tmp_path / "logs" / "workstation.log",
        mode="run",
    )

    assert command.startswith('"C:\\Program Files\\NetStar\\NetStarFileParseWorkstation.exe"')
    assert "--server" in command
    assert command.endswith(" run")


def test_doctor_reports_local_checks(tmp_path: Path) -> None:
    watch_path = tmp_path / "watch"
    watch_path.mkdir()
    config_path = tmp_path / "server.json"
    state_db = tmp_path / "state.db"
    log_file = tmp_path / "logs" / "workstation.log"
    save_config(
        config_path,
        WorkstationConfig(
            api_base_url="http://127.0.0.1:8080",
            mac="00:11:22:33:44:55",
            workstation_token="token",
            items=[{"deviceId": 1, "watchPath": str(watch_path), "fileType": "csv", "stableSeconds": 10}],
        ),
    )

    result = doctor(config_path, state_db, log_file, check_network=False)

    assert result["status"] == "ok"
    assert {check["name"] for check in result["checks"]} >= {"config", "token", "watchPath", "stateDb", "logDir"}


def test_doctor_reports_missing_token(tmp_path: Path) -> None:
    config_path = tmp_path / "server.json"
    save_config(
        config_path,
        WorkstationConfig(
            api_base_url="http://127.0.0.1:8080",
            mac="00:11:22:33:44:55",
        ),
    )

    result = doctor(config_path, tmp_path / "state.db", None, check_network=False)

    assert result["status"] == "failed"
    assert {"name": "token", "status": "failed", "message": "missing"} in result["checks"]


def test_state_store_tracks_file_lifecycle(tmp_path: Path) -> None:
    store = StateStore(tmp_path / "state.db")
    store.init()

    store.mark_uploading(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_name="A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        file_hash="sha256:test",
    )
    store.mark_uploaded(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        data_no="DATA-001",
    )
    store.apply_task_result({"dataNo": "DATA-001", "status": "success"})

    row = store.get_by_file(1, "/tmp/A001.csv", 3, "2026-06-30T10:20:30")
    assert row["status"] == "parse_success"
    assert row["retry_count"] == 0
    assert row["first_seen_time"]
    assert row["last_seen_time"]
    assert row["uploaded_time"]
    assert row["finished_time"]
    assert store.uploaded_data_nos() == []


def test_state_store_applies_status_compensation(tmp_path: Path) -> None:
    store = StateStore(tmp_path / "state.db")
    store.init()
    store.mark_uploading(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_name="A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        file_hash="sha256:test",
    )
    store.mark_uploaded(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        data_no="DATA-001",
    )

    updated_count = store.apply_statuses(
        [
            {"dataNo": "DATA-001", "status": "failed", "errorCode": "PARSE_FAILED", "errorMessage": "bad file"},
            {"dataNo": "DATA-002", "status": "pending"},
        ]
    )
    row = store.get_by_file(1, "/tmp/A001.csv", 3, "2026-06-30T10:20:30")

    assert updated_count == 1
    assert row["status"] == "parse_failed"
    assert row["last_error_code"] == "PARSE_FAILED"


def test_state_store_lists_records_and_failed_uploads(tmp_path: Path) -> None:
    store = StateStore(tmp_path / "state.db")
    store.init()

    store.mark_uploading(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_name="A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        file_hash="sha256:test",
    )
    store.mark_upload_failed(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        error_message="network failed",
    )

    rows = store.list_records()
    failed = store.failed_uploads()

    assert rows[0]["status"] == "upload_failed"
    assert rows[0]["retry_count"] == 1
    assert failed[0]["last_error_message"] == "network failed"


def test_state_store_recovers_interrupted_uploads(tmp_path: Path) -> None:
    store = StateStore(tmp_path / "state.db")
    store.init()
    store.mark_uploading(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_name="A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        file_hash="sha256:test",
    )

    assert store.recover_uploading() == 1
    row = store.list_records()[0]
    assert row["status"] == "upload_failed"
    assert row["last_error_code"] == "UPLOAD_INTERRUPTED"


def test_state_store_can_clear_only_failed_records(tmp_path: Path) -> None:
    store = StateStore(tmp_path / "state.db")
    store.init()
    store.mark_uploading(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_name="A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        file_hash="sha256:test",
    )
    store.mark_upload_failed(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        error_message="network failed",
    )
    store.mark_uploading(
        device_id=2,
        local_path="/tmp/A002.csv",
        file_name="A002.csv",
        file_size=4,
        file_mtime="2026-06-30T10:20:31",
        file_hash="sha256:test-2",
    )
    store.mark_uploaded(
        device_id=2,
        local_path="/tmp/A002.csv",
        file_size=4,
        file_mtime="2026-06-30T10:20:31",
        data_no="DATA-002",
    )

    assert store.clear_failed_records() == 1
    rows = store.list_records(limit=10)
    assert len(rows) == 1
    assert rows[0]["status"] == "uploaded"


def test_state_store_migrates_existing_database(tmp_path: Path) -> None:
    db_path = tmp_path / "state.db"
    db_path.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(db_path) as conn:
        conn.execute(
            """
            CREATE TABLE file_state (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              device_id INTEGER NOT NULL,
              local_path TEXT NOT NULL,
              file_name TEXT NOT NULL,
              file_size INTEGER NOT NULL,
              file_mtime TEXT NOT NULL,
              file_hash TEXT NOT NULL,
              data_no TEXT,
              status TEXT NOT NULL,
              last_error_code TEXT,
              last_error_message TEXT,
              created_time TEXT NOT NULL,
              updated_time TEXT NOT NULL,
              UNIQUE(device_id, local_path, file_size, file_mtime)
            )
            """
        )
        conn.execute(
            """
            INSERT INTO file_state (
              device_id, local_path, file_name, file_size, file_mtime, file_hash,
              status, created_time, updated_time
            ) VALUES (1, '/tmp/A001.csv', 'A001.csv', 3, '2026-06-30T10:20:30', 'sha256:test',
              'uploaded', '2026-06-30T10:20:30', '2026-06-30T10:20:30')
            """
        )

    store = StateStore(db_path)
    store.init()
    row = store.list_records()[0]

    assert row["retry_count"] == 0
    assert row["first_seen_time"] == "2026-06-30T10:20:30"
    assert row["last_seen_time"] == "2026-06-30T10:20:30"


def test_client_hash_and_multipart_body(tmp_path: Path) -> None:
    file_path = tmp_path / "A001.csv"
    file_path.write_bytes(b"abc")

    body = multipart_body({"deviceId": "1"}, "file", file_path, "boundary")

    assert sha256_file(file_path) == "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    assert b'name="deviceId"' in body
    assert b"A001.csv" in body
    assert b"abc" in body


def test_scanner_filters_file_type(tmp_path: Path) -> None:
    scanner = DirectoryScanner(
        WorkstationConfig(api_base_url="http://127.0.0.1:8080", mac="00:11:22:33:44:55"),
        api_client=None,
        state_store=None,
    )

    assert scanner._is_supported_file(tmp_path / "A001.csv", "csv") is True
    assert scanner._is_supported_file(tmp_path / "A001.xlsx", "csv") is False
    assert scanner._is_supported_file(tmp_path / "A001.xlsx", "excel") is True


def test_scanner_iter_watch_files_respects_recursive_and_max_depth(tmp_path: Path) -> None:
    (tmp_path / "root.csv").write_text("root", encoding="utf-8")
    child = tmp_path / "child"
    child.mkdir()
    (child / "child.csv").write_text("child", encoding="utf-8")
    grandchild = child / "grandchild"
    grandchild.mkdir()
    (grandchild / "grandchild.csv").write_text("grandchild", encoding="utf-8")
    scanner = DirectoryScanner(
        WorkstationConfig(api_base_url="http://127.0.0.1:8080", mac="00:11:22:33:44:55"),
        api_client=None,
        state_store=None,
    )

    flat = [path.name for path in scanner._iter_watch_files(tmp_path)]
    recursive = sorted(path.name for path in scanner._iter_watch_files(tmp_path, recursive=True))
    depth_one = sorted(path.name for path in scanner._iter_watch_files(tmp_path, recursive=True, max_depth=1))

    assert flat == ["child", "root.csv"]
    assert recursive == ["child", "child.csv", "grandchild", "grandchild.csv", "root.csv"]
    assert depth_one == ["child", "child.csv", "grandchild", "root.csv"]


def test_scanner_filters_temporary_files(tmp_path: Path) -> None:
    scanner = DirectoryScanner(
        WorkstationConfig(api_base_url="http://127.0.0.1:8080", mac="00:11:22:33:44:55"),
        api_client=None,
        state_store=None,
    )

    assert scanner._is_temporary_file(tmp_path / "~$A001.xlsx") is True
    assert scanner._is_temporary_file(tmp_path / "A001.csv.tmp") is True
    assert scanner._is_temporary_file(tmp_path / "A001.csv") is False


def test_scanner_waits_for_configured_stable_seconds(tmp_path: Path) -> None:
    scanner = DirectoryScanner(
        WorkstationConfig(api_base_url="http://127.0.0.1:8080", mac="00:11:22:33:44:55"),
        api_client=None,
        state_store=None,
    )

    assert scanner._is_stable("A001.csv", 3, 100.0, stable_seconds=10) is False
    assert scanner._is_stable("A001.csv", 3, 100.0, stable_seconds=10) is False
    scanner._snapshot["A001.csv"] = (3, 100.0, scanner._snapshot["A001.csv"][2] - 10)
    assert scanner._is_stable("A001.csv", 3, 100.0, stable_seconds=10) is True
    assert scanner._is_stable("A001.csv", 4, 101.0, stable_seconds=10) is False


def test_scanner_ignores_existing_files_until_changed(tmp_path: Path) -> None:
    file_path = tmp_path / "A001.csv"
    file_path.write_text("v1", encoding="utf-8")
    scanner = DirectoryScanner(
        WorkstationConfig(
            api_base_url="http://127.0.0.1:8080",
            mac="00:11:22:33:44:55",
            items=[
                {
                    "deviceId": 1,
                    "watchPath": str(tmp_path),
                    "fileType": "csv",
                    "stableSeconds": 2,
                    "enabled": True,
                }
            ],
        ),
        api_client=None,
        state_store=None,
    )

    scanner._initialize_baseline()
    scanner._scan_for_changes()

    assert str(file_path) not in scanner._pending_paths


def test_scanner_marks_changed_files_as_pending(tmp_path: Path) -> None:
    file_path = tmp_path / "A001.csv"
    file_path.write_text("v1", encoding="utf-8")
    scanner = DirectoryScanner(
        WorkstationConfig(
            api_base_url="http://127.0.0.1:8080",
            mac="00:11:22:33:44:55",
            items=[
                {
                    "deviceId": 1,
                    "watchPath": str(tmp_path),
                    "fileType": "csv",
                    "stableSeconds": 2,
                    "enabled": True,
                }
            ],
        ),
        api_client=None,
        state_store=None,
    )

    scanner._initialize_baseline()
    file_path.write_text("v22", encoding="utf-8")
    scanner._scan_for_changes()

    assert str(file_path) in scanner._pending_paths


def test_scanner_rebuilds_watchers_when_runtime_config_changes(tmp_path: Path, monkeypatch) -> None:
    scanner = DirectoryScanner(
        WorkstationConfig(
            api_base_url="http://127.0.0.1:8080",
            mac="00:11:22:33:44:55",
            items=[
                {
                    "deviceId": 1,
                    "watchPath": str(tmp_path),
                    "fileType": "csv",
                    "stableSeconds": 2,
                    "enabled": True,
                }
            ],
        ),
        api_client=None,
        state_store=None,
    )
    calls: list[str] = []
    monkeypatch.setattr(scanner, "_stop_watchdog", lambda: calls.append("stop"))
    monkeypatch.setattr(scanner, "_initialize_baseline", lambda: calls.append("init"))
    monkeypatch.setattr(scanner, "_start_watchdog_if_available", lambda: calls.append("start"))

    scanner.config.items = [
        {
            "deviceId": 2,
            "watchPath": str(tmp_path / "new-watch"),
            "fileType": "csv",
            "stableSeconds": 2,
            "enabled": True,
        }
    ]
    scanner._refresh_runtime_config()

    assert calls == ["stop", "init", "start"]


def test_websocket_config_full_is_persisted(tmp_path: Path) -> None:
    config_path = tmp_path / "server.json"
    config = WorkstationConfig(
        api_base_url="http://127.0.0.1:8080",
        mac="00:11:22:33:44:55",
        workstation_token="token",
    )
    save_config(config_path, config)
    ws_client = WebSocketClient(config, api_client=None, state_store=None, config_path=config_path)

    ws_client._apply_message(
        {
            "type": "config.full",
            "data": {
                "configVersion": 2,
                "items": [
                    {
                        "deviceId": 1,
                        "watchPath": str(tmp_path),
                        "fileType": "csv",
                        "stableSeconds": 5,
                        "enabled": True,
                    }
                ],
            },
        }
    )

    loaded = load_config(config_path)
    assert loaded.config_version == 2
    assert loaded.items[0]["stableSeconds"] == 5


def test_websocket_heartbeat_ack_is_ignored_while_task_result_updates_state(tmp_path: Path) -> None:
    config = WorkstationConfig(
        api_base_url="http://127.0.0.1:8080",
        mac="00:11:22:33:44:55",
        workstation_token="token",
    )
    store = StateStore(tmp_path / "state.db")
    store.init()
    store.mark_uploading(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_name="A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        file_hash="sha256:test",
    )
    store.mark_uploaded(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        data_no="DATA-001",
    )
    ws_client = WebSocketClient(config, api_client=None, state_store=store)

    ws_client._apply_message({"type": "heartbeat.ack"})
    ws_client._apply_message({"type": "task.result", "data": {"dataNo": "DATA-001", "status": "success"}})

    row = store.get_by_file(1, "/tmp/A001.csv", 3, "2026-06-30T10:20:30")
    assert row["status"] == "parse_success"


def test_websocket_doctor_run_executes_local_doctor_and_sends_result(tmp_path: Path, monkeypatch) -> None:
    config_path = tmp_path / "server.json"
    config = WorkstationConfig(
        api_base_url="http://127.0.0.1:8080",
        mac="00:11:22:33:44:55",
        workstation_token="token",
    )
    save_config(config_path, config)
    store = StateStore(tmp_path / "state.db")
    store.init()
    ws_client = WebSocketClient(config, api_client=None, state_store=store, config_path=config_path)

    monkeypatch.setattr(
        "workstation.ws_client.doctor",
        lambda *_args, **_kwargs: {
            "status": "ok",
            "checks": [{"name": "api", "status": "ok", "message": "ok"}],
        },
    )

    class FakeWebSocket:
        def __init__(self) -> None:
            self.payloads: list[str] = []

        async def send(self, payload: str) -> None:
            self.payloads.append(payload)

    websocket = FakeWebSocket()
    asyncio.run(
        ws_client._handle_message(
            websocket,
            {
                "type": "doctor.run",
                "messageId": "msg-doctor",
                "data": {"requestId": "doctor-001", "checkNetwork": True},
            },
        )
    )

    assert websocket.payloads
    payload = json.loads(websocket.payloads[0])
    assert payload["type"] == "doctor.result"
    assert payload["data"]["requestId"] == "doctor-001"
    assert payload["data"]["status"] == "ok"


def test_cli_upload_file_updates_local_state(tmp_path: Path, monkeypatch) -> None:
    config_path = tmp_path / "server.json"
    state_db = tmp_path / "state.db"
    file_path = tmp_path / "A001.csv"
    file_path.write_bytes(b"abc")
    save_config(
        config_path,
        WorkstationConfig(
            api_base_url="http://127.0.0.1:8080",
            mac="00:11:22:33:44:55",
            workstation_token="token",
        ),
    )

    def fake_upload_file(self, *, device_id: int, local_path: Path, file_mtime: datetime) -> dict:
        return {"dataNo": "DATA-001", "status": "pending"}

    monkeypatch.setattr("workstation.client.ApiClient.upload_file", fake_upload_file)

    result = asyncio.run(upload(config_path, state_db, 1, file_path))
    store = StateStore(state_db)
    row = store.list_records()[0]

    assert result == {"dataNo": "DATA-001", "status": "pending"}
    assert row["status"] == "uploaded"
    assert row["data_no"] == "DATA-001"


def test_cli_status_updates_local_state(tmp_path: Path, monkeypatch) -> None:
    config_path = tmp_path / "server.json"
    state_db = tmp_path / "state.db"
    save_config(
        config_path,
        WorkstationConfig(
            api_base_url="http://127.0.0.1:8080",
            mac="00:11:22:33:44:55",
            workstation_token="token",
        ),
    )
    store = StateStore(state_db)
    store.init()
    store.mark_uploading(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_name="A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        file_hash="sha256:test",
    )
    store.mark_uploaded(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        data_no="DATA-001",
    )

    def fake_push_status(self, data_nos: list[str]) -> list[dict]:
        assert data_nos == ["DATA-001"]
        return [{"dataNo": "DATA-001", "status": "success", "errorCode": None, "errorMessage": None}]

    monkeypatch.setattr("workstation.client.ApiClient.push_status", fake_push_status)

    result = query_status(config_path, state_db, [])
    row = store.get_by_file(1, "/tmp/A001.csv", 3, "2026-06-30T10:20:30")

    assert result[0]["status"] == "success"
    assert row["status"] == "parse_success"


def test_failed_upload_retrier_uploads_failed_record(tmp_path: Path, monkeypatch) -> None:
    state_db = tmp_path / "state.db"
    file_path = tmp_path / "A001.csv"
    file_path.write_bytes(b"abc")
    store = StateStore(state_db)
    store.init()
    store.mark_uploading(
        device_id=1,
        local_path=str(file_path),
        file_name=file_path.name,
        file_size=3,
        file_mtime=datetime.fromtimestamp(file_path.stat().st_mtime).isoformat(),
        file_hash="sha256:test",
    )
    store.mark_upload_failed(
        device_id=1,
        local_path=str(file_path),
        file_size=3,
        file_mtime=datetime.fromtimestamp(file_path.stat().st_mtime).isoformat(),
        error_message="network failed",
    )

    def fake_upload_file(self, *, device_id: int, local_path: Path, file_mtime: datetime) -> dict:
        return {"dataNo": "DATA-RETRY", "status": "pending"}

    monkeypatch.setattr("workstation.client.ApiClient.upload_file", fake_upload_file)
    retrier = FailedUploadRetrier(
        api_client=ApiClient("http://127.0.0.1:8080", "00:11:22:33:44:55", "token"),
        state_store=store,
        interval_seconds=1,
    )

    results = asyncio.run(retrier.retry_once())
    row = store.list_records()[0]

    assert results[0]["status"] == "uploaded"
    assert row["status"] == "uploaded"
    assert row["data_no"] == "DATA-RETRY"


def test_failed_upload_retrier_skips_max_retry_records(tmp_path: Path) -> None:
    store = StateStore(tmp_path / "state.db")
    store.init()
    store.mark_uploading(
        device_id=1,
        local_path="/tmp/A001.csv",
        file_name="A001.csv",
        file_size=3,
        file_mtime="2026-06-30T10:20:30",
        file_hash="sha256:test",
    )
    for _ in range(5):
        store.mark_upload_failed(
            device_id=1,
            local_path="/tmp/A001.csv",
            file_size=3,
            file_mtime="2026-06-30T10:20:30",
            error_message="network failed",
        )
    retrier = FailedUploadRetrier(
        api_client=ApiClient("http://127.0.0.1:8080", "00:11:22:33:44:55", "token"),
        state_store=store,
        max_retry_count=5,
    )

    results = asyncio.run(retrier.retry_once())

    assert results == [{"localPath": "/tmp/A001.csv", "status": "max_retry_reached"}]
