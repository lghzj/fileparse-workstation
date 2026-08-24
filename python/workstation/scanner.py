from __future__ import annotations

import asyncio
import hashlib
import json
import logging
from datetime import datetime
from pathlib import Path
from time import monotonic
from typing import Any

from workstation.client import ApiClient
from workstation.config import WorkstationConfig
from workstation.state import StateStore

logger = logging.getLogger(__name__)

try:
    from watchdog.events import FileSystemEventHandler
    from watchdog.observers import Observer
except ImportError:  # pragma: no cover - optional runtime dependency
    FileSystemEventHandler = object
    Observer = None


class DirectoryScanner:
    def __init__(self, config: WorkstationConfig, api_client: ApiClient, state_store: StateStore) -> None:
        self.config = config
        self.api_client = api_client
        self.state_store = state_store
        self._stopped = asyncio.Event()
        self._snapshot: dict[str, tuple[int, float, float]] = {}
        self._known_files: dict[str, tuple[int, float]] = {}
        self._pending_paths: set[str] = set()
        self._observer: Any = None
        self._config_signature = self._current_config_signature()

    async def run(self) -> None:
        self._initialize_baseline()
        self._start_watchdog_if_available()
        while not self._stopped.is_set():
            try:
                self._refresh_runtime_config()
                self._scan_for_changes()
                await self.scan_once()
            except Exception:
                logger.exception("scan loop failed")
            await asyncio.sleep(1)
        self._stop_watchdog()

    def stop(self) -> None:
        self._stopped.set()

    def _initialize_baseline(self) -> None:
        self._known_files = {}
        self._snapshot.clear()
        self._pending_paths.clear()
        for item in self.config.items:
            if not item.get("enabled"):
                continue
            for file_path in self._iter_item_files(item):
                stat = file_path.stat()
                self._known_files[str(file_path)] = (stat.st_size, stat.st_mtime)

    def _scan_for_changes(self) -> None:
        current_paths: set[str] = set()
        for item in self.config.items:
            if not item.get("enabled"):
                continue
            try:
                item_files = self._iter_item_files(item)
            except OSError:
                logger.exception("watch path scan failed watch_path=%s", item.get("watchPath"))
                continue
            for file_path in item_files:
                path_key = str(file_path)
                stat = file_path.stat()
                current_paths.add(path_key)
                current = (stat.st_size, stat.st_mtime)
                previous = self._known_files.get(path_key)
                if previous is None or previous != current:
                    self._known_files[path_key] = current
                    self._pending_paths.add(path_key)
                    logger.info("file change discovered local_path=%s", file_path)

        removed_paths = set(self._known_files) - current_paths
        for path_key in removed_paths:
            self._known_files.pop(path_key, None)
            self._pending_paths.discard(path_key)
            self._snapshot.pop(path_key, None)

    async def scan_once(self) -> None:
        for item in self.config.items:
            if not item.get("enabled"):
                continue
            device_id = int(item["deviceId"])
            for file_path in self._iter_item_files(item):
                key = str(file_path)
                if key not in self._pending_paths:
                    continue
                stat = file_path.stat()
                file_mtime = datetime.fromtimestamp(stat.st_mtime).isoformat()
                stable_seconds = int(item.get("stableSeconds") or 2)
                if not self._is_stable(key, stat.st_size, stat.st_mtime, stable_seconds):
                    continue
                if await self._already_uploaded(device_id, file_path, stat.st_size, file_mtime):
                    self._pending_paths.discard(key)
                    continue
                await upload_file_once(self.api_client, self.state_store, device_id, file_path)
                self._pending_paths.discard(key)

    def _iter_item_files(self, item: dict) -> list[Path]:
        watch_path = Path(item["watchPath"])
        if not watch_path.exists() or not watch_path.is_dir():
            logger.warning("watch path unavailable watch_path=%s", watch_path)
            return []
        files: list[Path] = []
        for file_path in self._iter_watch_files(
            watch_path,
            recursive=bool(item.get("recursive")),
            max_depth=int(item.get("maxDepth") or 0),
        ):
            if not file_path.is_file():
                continue
            if self._is_temporary_file(file_path):
                continue
            if not self._is_supported_file(file_path, str(item["fileType"])):
                continue
            files.append(file_path)
        return files

    def _iter_watch_files(self, watch_path: Path, *, recursive: bool = False, max_depth: int = 0) -> list[Path]:
        if not recursive:
            return sorted(watch_path.iterdir())
        files: list[Path] = []
        for path in watch_path.rglob("*"):
            try:
                relative = path.relative_to(watch_path)
            except ValueError:
                continue
            depth = len(relative.parts) - 1
            if max_depth > 0 and depth > max_depth:
                continue
            files.append(path)
        return sorted(files)

    def _is_supported_file(self, file_path: Path, file_type: str) -> bool:
        suffix = file_path.suffix.lower().lstrip(".")
        allowed = {
            "word": {"doc", "docx"},
            "excel": {"xls", "xlsx", "csv"},
            "csv": {"csv"},
            "ppt": {"ppt", "pptx"},
            "pdf": {"pdf"},
            "txt": {"txt"},
        }
        return suffix in allowed.get(file_type.lower(), set())

    def _is_temporary_file(self, file_path: Path) -> bool:
        name = file_path.name.lower()
        suffix = file_path.suffix.lower()
        if name.startswith("~$") or name.startswith("."):
            return True
        if suffix in {".tmp", ".temp", ".part", ".crdownload", ".download", ".swp", ".bak"}:
            return True
        return name.endswith(".filepart")

    def _is_stable(self, key: str, file_size: int, file_mtime: float, stable_seconds: int) -> bool:
        now = monotonic()
        snapshot = self._snapshot.get(key)
        if snapshot is None or snapshot[0] != file_size or snapshot[1] != file_mtime:
            self._snapshot[key] = (file_size, file_mtime, now)
            return False
        return now - snapshot[2] >= stable_seconds

    def _start_watchdog_if_available(self) -> None:
        if Observer is None:
            logger.info("watchdog is not installed, falling back to lightweight polling")
            return

        watch_paths: list[tuple[Path, bool]] = []
        seen_paths: set[str] = set()
        for item in self.config.items:
            if not item.get("enabled"):
                continue
            watch_path = Path(item["watchPath"])
            if not watch_path.exists() or not watch_path.is_dir():
                continue
            path_key = str(watch_path.resolve())
            if path_key in seen_paths:
                continue
            seen_paths.add(path_key)
            watch_paths.append((watch_path, bool(item.get("recursive"))))

        if not watch_paths:
            return

        observer = Observer()
        handler = _WatchPathHandler(self)
        for watch_path, recursive in watch_paths:
            observer.schedule(handler, str(watch_path), recursive=recursive)
        observer.start()
        self._observer = observer
        logger.info("watchdog observer started watch_paths=%s", len(watch_paths))

    def _stop_watchdog(self) -> None:
        observer = self._observer
        if observer is None:
            return
        observer.stop()
        observer.join(timeout=3)
        self._observer = None

    def _refresh_runtime_config(self) -> None:
        current_signature = self._current_config_signature()
        if current_signature == self._config_signature:
            return
        self._config_signature = current_signature
        logger.info("scanner config changed, rebuilding watchers")
        self._stop_watchdog()
        self._initialize_baseline()
        self._start_watchdog_if_available()

    def _current_config_signature(self) -> str:
        return json.dumps(self.config.items, ensure_ascii=False, sort_keys=True)

    def mark_path_dirty(self, file_path: str) -> None:
        path = Path(file_path)
        if not path.exists() or not path.is_file():
            return
        if self._is_temporary_file(path):
            return
        self._pending_paths.add(str(path))

    async def _already_uploaded(self, device_id: int, file_path: Path, file_size: int, file_mtime: str) -> bool:
        row = self.state_store.get_by_file(device_id, str(file_path), file_size, file_mtime)
        return row is not None and row["data_no"] is not None

    def _sha256_file(self, path: Path) -> str:
        hasher = hashlib.sha256()
        with path.open("rb") as file:
            for chunk in iter(lambda: file.read(1024 * 1024), b""):
                hasher.update(chunk)
        return f"sha256:{hasher.hexdigest()}"


async def upload_file_once(api_client: ApiClient, state_store: StateStore, device_id: int, file_path: Path) -> dict | None:
    file_stat = file_path.stat()
    file_mtime = datetime.fromtimestamp(file_stat.st_mtime).isoformat()
    try:
        file_hash = sha256_file(file_path)
        state_store.mark_uploading(
            device_id=device_id,
            local_path=str(file_path),
            file_name=file_path.name,
            file_size=file_stat.st_size,
            file_mtime=file_mtime,
            file_hash=file_hash,
        )
        result = api_client.upload_file(
            device_id=device_id,
            local_path=file_path,
            file_mtime=datetime.fromtimestamp(file_stat.st_mtime),
        )
        state_store.mark_uploaded(
            device_id=device_id,
            local_path=str(file_path),
            file_size=file_stat.st_size,
            file_mtime=file_mtime,
            data_no=result["dataNo"],
        )
        logger.info("file uploaded local_path=%s data_no=%s", file_path, result["dataNo"])
        return result
    except Exception as exc:
        state_store.mark_upload_failed(
            device_id=device_id,
            local_path=str(file_path),
            file_size=file_stat.st_size,
            file_mtime=file_mtime,
            error_message=str(exc),
        )
        logger.exception("upload failed local_path=%s", file_path)
        return None


def sha256_file(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            hasher.update(chunk)
    return f"sha256:{hasher.hexdigest()}"


class _WatchPathHandler(FileSystemEventHandler):
    def __init__(self, scanner: DirectoryScanner) -> None:
        self.scanner = scanner

    def on_created(self, event) -> None:
        if not event.is_directory:
            self.scanner.mark_path_dirty(event.src_path)

    def on_modified(self, event) -> None:
        if not event.is_directory:
            self.scanner.mark_path_dirty(event.src_path)

    def on_moved(self, event) -> None:
        if not event.is_directory:
            self.scanner.mark_path_dirty(event.dest_path)
