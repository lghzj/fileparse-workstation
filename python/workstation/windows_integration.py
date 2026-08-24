from __future__ import annotations

import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

from workstation.config import load_config
from workstation.state import StateStore

RUN_KEY_PATH = r"Software\Microsoft\Windows\CurrentVersion\Run"
STARTUP_VALUE_NAME = "NetStarFileParseWorkstation"


def startup_command(
    executable: str | None = None,
    *,
    config_path: Path,
    state_db: Path,
    log_file: Path | None = None,
    mode: str = "gui",
) -> str:
    exe = executable or sys.executable
    parts = [
        _quote(exe),
        "--server",
        _quote(str(config_path)),
        "--state-db",
        _quote(str(state_db)),
    ]
    if log_file is not None:
        parts.extend(["--log-file", _quote(str(log_file))])
    parts.append(mode)
    return " ".join(parts)


def install_startup(config_path: Path, state_db: Path, log_file: Path | None = None, *, mode: str = "gui") -> dict:
    if os.name != "nt":
        raise RuntimeError("Windows startup registration is only supported on Windows")
    import winreg

    command = startup_command(config_path=config_path, state_db=state_db, log_file=log_file, mode=mode)
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY_PATH, 0, winreg.KEY_SET_VALUE) as key:
        winreg.SetValueEx(key, STARTUP_VALUE_NAME, 0, winreg.REG_SZ, command)
    return {"name": STARTUP_VALUE_NAME, "command": command}


def uninstall_startup() -> dict:
    if os.name != "nt":
        raise RuntimeError("Windows startup registration is only supported on Windows")
    import winreg

    removed = False
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY_PATH, 0, winreg.KEY_SET_VALUE) as key:
        try:
            winreg.DeleteValue(key, STARTUP_VALUE_NAME)
            removed = True
        except FileNotFoundError:
            removed = False
    return {"name": STARTUP_VALUE_NAME, "removed": removed}


def doctor(config_path: Path, state_db: Path, log_file: Path | None = None, *, check_network: bool = True) -> dict:
    checks: list[dict] = []
    config = None
    try:
        config = load_config(config_path)
        checks.append(_ok("config", str(config_path)))
    except Exception as exc:
        checks.append(_fail("config", str(exc)))

    if config is not None:
        checks.append(_ok("token", "configured") if config.workstation_token else _fail("token", "missing"))
        for item in config.items:
            watch_path = Path(item.get("watchPath") or "")
            if watch_path.exists() and watch_path.is_dir():
                readable = os.access(watch_path, os.R_OK)
                checks.append(_ok("watchPath", str(watch_path)) if readable else _fail("watchPath", f"not readable: {watch_path}"))
            else:
                checks.append(_fail("watchPath", f"not found: {watch_path}"))
        if check_network:
            checks.append(_check_http(config.api_base_url))
        else:
            checks.append(_skip("api", "network check skipped"))

    try:
        StateStore(state_db).init()
        checks.append(_ok("stateDb", str(state_db)))
    except Exception as exc:
        checks.append(_fail("stateDb", str(exc)))

    if log_file is not None:
        try:
            log_file.parent.mkdir(parents=True, exist_ok=True)
            probe = log_file.parent / ".write-test"
            probe.write_text("ok", encoding="utf-8")
            probe.unlink()
            checks.append(_ok("logDir", str(log_file.parent)))
        except Exception as exc:
            checks.append(_fail("logDir", str(exc)))

    status = "ok" if all(check["status"] in {"ok", "skipped"} for check in checks) else "failed"
    return {"status": status, "checks": checks}


def _check_http(api_base_url: str) -> dict:
    url = api_base_url.rstrip("/") + "/api/fileparse/health"
    try:
        with urllib.request.urlopen(url, timeout=5) as response:
            if response.status < 400:
                return _ok("api", url)
            return _fail("api", f"HTTP {response.status}: {url}")
    except urllib.error.URLError as exc:
        return _fail("api", str(exc))


def _quote(value: str) -> str:
    escaped = value.replace('"', r"\"")
    return f'"{escaped}"'


def _ok(name: str, message: str) -> dict:
    return {"name": name, "status": "ok", "message": message}


def _fail(name: str, message: str) -> dict:
    return {"name": name, "status": "failed", "message": message}


def _skip(name: str, message: str) -> dict:
    return {"name": name, "status": "skipped", "message": message}
