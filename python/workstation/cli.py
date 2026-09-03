from __future__ import annotations

import argparse
import asyncio
import json
import logging
import sys
from pathlib import Path

from workstation.build_info import runtime_summary
from workstation.client import ApiClient
from workstation.config import WorkstationConfig, load_config, save_config
from workstation.scanner import DirectoryScanner, upload_file_once
from workstation.runtime import FailedUploadRetrier
from workstation.state import StateStore
from workstation.ws_client import WebSocketClient
from workstation.windows_integration import doctor, install_startup, uninstall_startup

logger = logging.getLogger(__name__)


def default_data_dir() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path("workstation")


def configure_logging(level: str, log_file: Path | None = None) -> None:
    handlers: list[logging.Handler] = []
    if sys.stderr is not None:
        handlers.append(logging.StreamHandler())
    if log_file is not None:
        log_file.parent.mkdir(parents=True, exist_ok=True)
        handlers.append(logging.FileHandler(log_file, encoding="utf-8"))
    if not handlers:
        handlers.append(logging.NullHandler())
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s [%(name)s] %(message)s",
        handlers=handlers,
        force=True,
    )
    logger.info("logging initialized log_file=%s frozen=%s", log_file, bool(getattr(sys, "frozen", False)))


def build_client(config: WorkstationConfig) -> ApiClient:
    return ApiClient(config.api_base_url, config.mac, config.workstation_token)


def register(config_path: Path) -> dict:
    config = load_config(config_path)
    client = build_client(config)
    data = client.register(ip=config.ip, hostname=config.hostname)
    config.workstation_token = client.workstation_token
    config.workstation_id = data["workstationId"]
    config.heartbeat_interval_seconds = data["heartbeatIntervalSeconds"]
    config.config_version = data["configVersion"]
    if data.get("wsUrl"):
        config.ws_url = data["wsUrl"]
    save_config(config_path, config)
    return data


def pull_config(config_path: Path) -> dict:
    config = load_config(config_path)
    config.validate(require_token=True)
    config.require_token()
    client = build_client(config)
    data = client.pull_config()
    config.config_version = data["configVersion"]
    config.items = list(data.get("items") or [])
    save_config(config_path, config)
    return data


def apply_remote_config(config: WorkstationConfig, data: dict, config_path: Path) -> bool:
    config_version = data.get("configVersion")
    items = list(data.get("items") or [])
    if config.config_version == config_version and config.items == items:
        return False
    config.config_version = config_version
    config.items = items
    save_config(config_path, config)
    logger.info("HTTP config synchronized version=%s items=%s", config.config_version, len(config.items))
    return True


async def sync_config_loop(
    config: WorkstationConfig,
    client: ApiClient,
    config_path: Path,
    *,
    interval_seconds: int = 5,
) -> None:
    while True:
        try:
            apply_remote_config(config, client.pull_config(), config_path)
        except Exception:
            logger.exception("HTTP config synchronization failed")
        await asyncio.sleep(interval_seconds)


def query_status(config_path: Path, state_db: Path, data_nos: list[str]) -> dict:
    config = load_config(config_path)
    config.validate(require_token=True)
    config.require_token()
    client = build_client(config)
    store = StateStore(state_db)
    store.init()
    if not data_nos:
        data_nos = store.uploaded_data_nos()
    if not data_nos:
        return []
    statuses = client.push_status(data_nos)
    store.apply_statuses(statuses)
    return statuses


async def upload(config_path: Path, state_db: Path, device_id: int, file_path: Path) -> dict | None:
    config = load_config(config_path)
    config.validate(require_token=True)
    config.require_token()
    store = StateStore(state_db)
    store.init()
    return await upload_file_once(build_client(config), store, device_id, file_path)


def list_local_state(state_db: Path, *, status: str | None = None, limit: int = 100) -> list[dict]:
    store = StateStore(state_db)
    store.init()
    return store.list_records(status=status, limit=limit)


async def retry_failed(config_path: Path, state_db: Path, limit: int = 100) -> list[dict]:
    config = load_config(config_path)
    config.validate(require_token=True)
    config.require_token()
    client = build_client(config)
    store = StateStore(state_db)
    store.init()
    retrier = FailedUploadRetrier(client, store, batch_size=limit)
    return await retrier.retry_once()


async def run(config_path: Path, state_db: Path) -> None:
    logger.info("workstation runtime starting config_path=%s state_db=%s", config_path, state_db)
    config = load_config(config_path)
    store = StateStore(state_db)
    store.init()
    recovered_count = store.recover_uploading()
    if recovered_count:
        logger.warning("recovered interrupted uploads count=%s", recovered_count)

    client = build_client(config)
    data = client.register(ip=config.ip, hostname=config.hostname)
    config.workstation_token = client.workstation_token
    config.workstation_id = data["workstationId"]
    config.heartbeat_interval_seconds = data["heartbeatIntervalSeconds"]
    config.config_version = data["configVersion"]
    if data.get("wsUrl"):
        config.ws_url = data["wsUrl"]

    config.validate(require_token=True)

    data = client.pull_config()
    config.config_version = data["configVersion"]
    config.items = list(data.get("items") or [])
    save_config(config_path, config)
    logger.info("workstation started config_version=%s items=%s", config.config_version, len(config.items))

    ws_client = WebSocketClient(config, client, store, config_path)
    scanner = DirectoryScanner(config, client, store)
    retrier = FailedUploadRetrier(client, store)
    await asyncio.gather(ws_client.run(), scanner.run(), retrier.run(), sync_config_loop(config, client, config_path))


def main() -> None:
    parser = argparse.ArgumentParser(description="NetStar IFSP workstation client")
    data_dir = default_data_dir()
    parser.add_argument("--server", default=str(data_dir / "server.json"))
    parser.add_argument("--state-db", default=str(data_dir / "state.db"))
    parser.add_argument("--log-level", default="INFO")
    parser.add_argument("--log-file")
    subparsers = parser.add_subparsers(dest="command")
    subparsers.add_parser("register")
    subparsers.add_parser("config")
    subparsers.add_parser("run")
    subparsers.add_parser("gui")
    doctor_parser = subparsers.add_parser("doctor")
    doctor_parser.add_argument("--skip-network", action="store_true")
    install_startup_parser = subparsers.add_parser("install-startup")
    install_startup_parser.add_argument("--mode", choices=["gui", "run"], default="gui")
    subparsers.add_parser("uninstall-startup")
    upload_parser = subparsers.add_parser("upload")
    upload_parser.add_argument("--device-id", type=int, required=True)
    upload_parser.add_argument("--file", required=True)
    status = subparsers.add_parser("status")
    status.add_argument("data_no", nargs="*")
    state = subparsers.add_parser("state")
    state.add_argument("--status")
    state.add_argument("--limit", type=int, default=100)
    retry_failed_parser = subparsers.add_parser("retry-failed")
    retry_failed_parser.add_argument("--limit", type=int, default=100)
    args = parser.parse_args()
    if args.command is None:
        args.command = "gui"

    config_path = Path(args.server).expanduser().resolve()
    state_db = Path(args.state_db).expanduser().resolve()
    log_file = (Path(args.log_file) if args.log_file else state_db.parent / "logs" / "workstation.log").expanduser().resolve()
    configure_logging(args.log_level, log_file)
    logger.info("workstation build %s", runtime_summary(args.command))

    if args.command == "register":
        print(json.dumps(register(config_path), ensure_ascii=False, indent=2))
    elif args.command == "config":
        print(json.dumps(pull_config(config_path), ensure_ascii=False, indent=2))
    elif args.command == "status":
        print(json.dumps(query_status(config_path, state_db, args.data_no), ensure_ascii=False, indent=2))
    elif args.command == "upload":
        print(
            json.dumps(
                asyncio.run(upload(config_path, state_db, args.device_id, Path(args.file))),
                ensure_ascii=False,
                indent=2,
            )
        )
    elif args.command == "state":
        print(json.dumps(list_local_state(state_db, status=args.status, limit=args.limit), ensure_ascii=False, indent=2))
    elif args.command == "retry-failed":
        print(json.dumps(asyncio.run(retry_failed(config_path, state_db, args.limit)), ensure_ascii=False, indent=2))
    elif args.command == "run":
        try:
            asyncio.run(run(config_path, state_db))
        except Exception:
            logger.exception("workstation runtime terminated unexpectedly")
            raise
    elif args.command == "gui":
        from workstation.gui import run_gui

        raise SystemExit(run_gui(config_path, state_db, log_file))
    elif args.command == "doctor":
        print(json.dumps(doctor(config_path, state_db, log_file, check_network=not args.skip_network), ensure_ascii=False, indent=2))
    elif args.command == "install-startup":
        print(json.dumps(install_startup(config_path, state_db, log_file, mode=args.mode), ensure_ascii=False, indent=2))
    elif args.command == "uninstall-startup":
        print(json.dumps(uninstall_startup(), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
