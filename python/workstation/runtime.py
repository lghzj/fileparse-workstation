from __future__ import annotations

import asyncio
import logging
from pathlib import Path

from workstation.client import ApiClient
from workstation.scanner import upload_file_once
from workstation.state import StateStore

logger = logging.getLogger(__name__)


class FailedUploadRetrier:
    def __init__(
        self,
        api_client: ApiClient,
        state_store: StateStore,
        *,
        interval_seconds: int = 60,
        batch_size: int = 20,
        max_retry_count: int = 5,
    ) -> None:
        self.api_client = api_client
        self.state_store = state_store
        self.interval_seconds = interval_seconds
        self.batch_size = batch_size
        self.max_retry_count = max_retry_count
        self._stopped = asyncio.Event()

    async def run(self) -> None:
        while not self._stopped.is_set():
            try:
                await self.retry_once()
            except Exception:
                logger.exception("failed upload retry loop failed")
            try:
                await asyncio.wait_for(self._stopped.wait(), timeout=self.interval_seconds)
            except asyncio.TimeoutError:
                continue

    def stop(self) -> None:
        self._stopped.set()

    async def retry_once(self) -> list[dict]:
        results: list[dict] = []
        for row in self.state_store.failed_uploads(limit=self.batch_size):
            if int(row["retry_count"] or 0) >= self.max_retry_count:
                results.append({"localPath": row["local_path"], "status": "max_retry_reached"})
                continue
            file_path = Path(row["local_path"])
            if not file_path.exists() or not file_path.is_file():
                results.append({"localPath": str(file_path), "status": "missing"})
                continue
            result = await upload_file_once(self.api_client, self.state_store, int(row["device_id"]), file_path)
            results.append({"localPath": str(file_path), "status": "uploaded" if result else "failed", "data": result})
        return results
