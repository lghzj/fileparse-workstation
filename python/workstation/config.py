from __future__ import annotations

import json
import socket
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import urlparse


class ConfigError(ValueError):
    pass


@dataclass
class WorkstationConfig:
    api_base_url: str
    mac: str
    ip: str | None = None
    hostname: str | None = None
    workstation_token: str | None = None
    workstation_id: int | None = None
    ws_url: str | None = None
    heartbeat_interval_seconds: int = 30
    config_version: int | None = None
    system_client_id: str = "business-system"
    system_token: str | None = None
    system_operator: str = "admin"
    items: list[dict] = field(default_factory=list)

    @classmethod
    def from_dict(cls, data: dict) -> "WorkstationConfig":
        return cls(
            api_base_url=data["apiBaseUrl"],
            mac=data.get("mac") or default_mac(),
            ip=data.get("ip"),
            hostname=data.get("hostname") or socket.gethostname(),
            workstation_token=data.get("workstationToken"),
            workstation_id=data.get("workstationId"),
            ws_url=data.get("wsUrl"),
            heartbeat_interval_seconds=int(data.get("heartbeatIntervalSeconds") or 30),
            config_version=data.get("configVersion"),
            system_client_id=data.get("systemClientId") or "business-system",
            system_token=data.get("systemToken"),
            system_operator=data.get("systemOperator") or "admin",
            items=list(data.get("items") or []),
        )

    def to_dict(self) -> dict:
        data = {
            "apiBaseUrl": self.api_base_url,
            "mac": self.mac,
            "ip": self.ip,
            "hostname": self.hostname,
            "heartbeatIntervalSeconds": self.heartbeat_interval_seconds,
        }
        if self.workstation_token:
            data["workstationToken"] = self.workstation_token
        if self.workstation_id is not None:
            data["workstationId"] = self.workstation_id
        if self.ws_url:
            data["wsUrl"] = self.ws_url
        if self.config_version is not None:
            data["configVersion"] = self.config_version
        if self.system_client_id:
            data["systemClientId"] = self.system_client_id
        if self.system_token:
            data["systemToken"] = self.system_token
        if self.system_operator:
            data["systemOperator"] = self.system_operator
        if self.items:
            data["items"] = self.items
        return data

    def resolved_ws_url(self) -> str:
        if not self.ws_url:
            parsed = urlparse(self.api_base_url)
            return f"ws://{parsed.netloc}/file"
        if self.ws_url.startswith("/"):
            parsed = urlparse(self.api_base_url)
            return f"ws://{parsed.netloc}{self.ws_url}"
        return self.ws_url

    def require_token(self) -> str:
        if not self.workstation_token:
            raise RuntimeError("workstationToken is missing, run register first")
        return self.workstation_token

    def validate(self, *, require_token: bool = False) -> None:
        errors: list[str] = []
        parsed_api = urlparse(self.api_base_url)
        if parsed_api.scheme not in {"http", "https"} or not parsed_api.netloc:
            errors.append("apiBaseUrl must be an absolute http(s) URL")
        if not self.mac or self.mac == "replace-with-workstation-mac":
            errors.append("mac must be configured")
        if self.heartbeat_interval_seconds <= 0:
            errors.append("heartbeatIntervalSeconds must be greater than 0")
        if require_token and not self.workstation_token:
            errors.append("workstationToken is missing, run register first")
        if self.ws_url:
            parsed_ws = urlparse(self.resolved_ws_url())
            if parsed_ws.scheme not in {"ws", "wss"} or not parsed_ws.netloc:
                errors.append("wsUrl must be an absolute ws(s) URL or a path")
        for index, item in enumerate(self.items):
            prefix = f"items[{index}]"
            if "deviceId" not in item:
                errors.append(f"{prefix}.deviceId is required")
            if not item.get("watchPath"):
                errors.append(f"{prefix}.watchPath is required")
            if not item.get("fileType"):
                errors.append(f"{prefix}.fileType is required")
            if int(item.get("stableSeconds") or 2) <= 0:
                errors.append(f"{prefix}.stableSeconds must be greater than 0")
            if int(item.get("maxDepth") or 0) < 0:
                errors.append(f"{prefix}.maxDepth must be greater than or equal to 0")
        if errors:
            raise ConfigError("; ".join(errors))


def default_mac() -> str:
    value = uuid.getnode()
    return ":".join(f"{(value >> offset) & 0xFF:02x}" for offset in range(40, -1, -8))


def default_ip() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            value = sock.getsockname()[0]
            if value and not value.startswith("127."):
                return value
    except OSError:
        pass

    try:
        for _, _, addresses in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            value = addresses[0]
            if value and not value.startswith("127."):
                return value
    except OSError:
        pass
    return "127.0.0.1"


def load_config(path: Path) -> WorkstationConfig:
    config = WorkstationConfig.from_dict(json.loads(path.read_text(encoding="utf-8")))
    config.validate()
    return config


def default_config() -> WorkstationConfig:
    return WorkstationConfig(
        api_base_url="http://127.0.0.1:8080",
        mac=default_mac(),
        ip=default_ip(),
        hostname=socket.gethostname(),
    )


def ensure_config(path: Path) -> WorkstationConfig:
    if path.exists():
        return load_config(path)
    config = default_config()
    save_config(path, config)
    return config


def save_config(path: Path, config: WorkstationConfig) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(config.to_dict(), ensure_ascii=False, indent=2), encoding="utf-8")


def update_base_config(
    path: Path,
    *,
    api_base_url: str,
    mac: str,
    ip: str | None = None,
    hostname: str | None = None,
    workstation_token: str | None = None,
    ws_url: str | None = None,
    heartbeat_interval_seconds: int = 30,
) -> WorkstationConfig:
    existing = load_config(path) if path.exists() else None
    config = WorkstationConfig(
        api_base_url=api_base_url.strip(),
        mac=mac.strip(),
        ip=ip.strip() if ip else None,
        hostname=hostname.strip() if hostname else None,
        workstation_token=workstation_token.strip() if workstation_token else None,
        workstation_id=existing.workstation_id if existing else None,
        ws_url=ws_url.strip() if ws_url else None,
        heartbeat_interval_seconds=heartbeat_interval_seconds,
        config_version=existing.config_version if existing else None,
        system_client_id=existing.system_client_id if existing else "business-system",
        system_token=existing.system_token if existing else None,
        system_operator=existing.system_operator if existing else "admin",
        items=existing.items if existing else [],
    )
    config.validate()
    save_config(path, config)
    return config
