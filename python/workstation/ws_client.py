from __future__ import annotations

import asyncio
import inspect
import json
import logging
from datetime import datetime
from pathlib import Path
from uuid import uuid4

import websockets

from workstation.client import ApiClient
from workstation.config import WorkstationConfig, save_config
from workstation.state import StateStore
from workstation.windows_integration import doctor

logger = logging.getLogger(__name__)


class WebSocketClient:
    def __init__(
        self,
        config: WorkstationConfig,
        api_client: ApiClient,
        state_store: StateStore,
        config_path: Path | None = None,
    ) -> None:
        self.config = config
        self.api_client = api_client
        self.state_store = state_store
        self.config_path = config_path
        self._stopped = asyncio.Event()

    async def run(self) -> None:
        while not self._stopped.is_set():
            try:
                await self._run_once()
            except Exception:
                logger.exception("websocket loop failed")
                await asyncio.sleep(5)

    def stop(self) -> None:
        self._stopped.set()

    async def _run_once(self) -> None:
        headers = {
            "Authorization": f"Bearer {self.api_client.workstation_token}",
            "X-Workstation-Mac": self.config.mac,
        }
        connect_parameters = inspect.signature(websockets.connect).parameters
        header_argument = "additional_headers" if "additional_headers" in connect_parameters else "extra_headers"
        logger.info("websocket connecting url=%s", self.config.resolved_ws_url())
        async with websockets.connect(
            self.config.resolved_ws_url(),
            ping_interval=None,
            **{header_argument: headers},
        ) as websocket:
            message = json.loads(await websocket.recv())
            await self._handle_message(websocket, message)
            logger.info("websocket connected message_type=%s", message.get("type"))
            receiver = asyncio.create_task(self._receive_messages(websocket))
            heartbeat = asyncio.create_task(self._heartbeat_loop(websocket))
            done, pending = await asyncio.wait(
                {receiver, heartbeat},
                return_when=asyncio.FIRST_EXCEPTION,
            )
            for task in pending:
                task.cancel()
            for task in done:
                task.result()

    async def _receive_messages(self, websocket) -> None:
        while not self._stopped.is_set():
            message = json.loads(await websocket.recv())
            if message.get("type") == "heartbeat.ack":
                logger.debug("heartbeat ack received")
                continue
            await self._handle_message(websocket, message)

    async def _heartbeat_loop(self, websocket) -> None:
        while not self._stopped.is_set():
            heartbeat_id = f"msg-{uuid4().hex}"
            await websocket.send(
                json.dumps(
                    {
                        "type": "heartbeat",
                        "messageId": heartbeat_id,
                        "timestamp": datetime.now().isoformat(),
                        "data": {"mac": self.config.mac, "ip": self.config.ip},
                    },
                    ensure_ascii=False,
                )
            )
            try:
                await asyncio.wait_for(self._stopped.wait(), timeout=self.config.heartbeat_interval_seconds)
            except asyncio.TimeoutError:
                continue

    def _apply_message(self, message: dict) -> None:
        message_type = message.get("type")
        if message_type == "heartbeat.ack":
            logger.debug("heartbeat ack received")
            return
        if message_type == "config.full":
            data = message.get("data") or {}
            self.config.config_version = data.get("configVersion")
            self.config.items = list(data.get("items") or [])
            if self.config_path is not None:
                save_config(self.config_path, self.config)
            logger.info("config updated version=%s items=%s", self.config.config_version, len(self.config.items))
            return
        if message_type == "task.result":
            data = message.get("data") or {}
            self.state_store.apply_task_result(data)
            logger.info("task result applied data_no=%s status=%s", data.get("dataNo"), data.get("status"))
            return
        logger.warning("unknown websocket message type=%s", message_type)

    async def _handle_message(self, websocket, message: dict) -> None:
        if message.get("type") == "doctor.run":
            await self._run_doctor(websocket, message)
            return
        self._apply_message(message)

    async def _run_doctor(self, websocket, message: dict) -> None:
        if self.config_path is None:
            logger.warning("doctor.run ignored because config_path is missing")
            return
        payload = message.get("data") or {}
        check_network = bool(payload.get("checkNetwork", True))
        log_file = self.config_path.parent / "logs" / "workstation.log"
        result = doctor(self.config_path, self.state_store.path, log_file, check_network=check_network)
        await websocket.send(
            json.dumps(
                {
                    "type": "doctor.result",
                    "messageId": message.get("messageId"),
                    "timestamp": datetime.now().isoformat(),
                    "data": {
                        "requestId": payload.get("requestId"),
                        "status": result.get("status"),
                        "checks": result.get("checks", []),
                    },
                },
                ensure_ascii=False,
            )
        )
