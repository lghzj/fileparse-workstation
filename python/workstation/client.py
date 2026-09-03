from __future__ import annotations

import hashlib
import json
import mimetypes
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path
from uuid import uuid4


def request_json(method: str, url: str, *, headers: dict | None = None, body: dict | None = None) -> dict:
    payload = json.dumps(body).encode("utf-8") if body is not None else None
    request = urllib.request.Request(url, data=payload, method=method, headers=headers or {})
    if payload is not None:
        request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raise RuntimeError(exc.read().decode("utf-8")) from exc


def sha256_file(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            hasher.update(chunk)
    return f"sha256:{hasher.hexdigest()}"


def multipart_body(fields: dict[str, str], file_field: str, file_path: Path, boundary: str) -> bytes:
    chunks: list[bytes] = []
    for name, value in fields.items():
        chunks.append(f"--{boundary}\r\n".encode())
        chunks.append(f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode())
        chunks.append(f"{value}\r\n".encode())
    content_type = mimetypes.guess_type(file_path.name)[0] or "application/octet-stream"
    chunks.append(f"--{boundary}\r\n".encode())
    chunks.append(
        f'Content-Disposition: form-data; name="{file_field}"; filename="{file_path.name}"\r\n'.encode()
    )
    chunks.append(f"Content-Type: {content_type}\r\n\r\n".encode())
    chunks.append(file_path.read_bytes())
    chunks.append(f"\r\n--{boundary}--\r\n".encode())
    return b"".join(chunks)


def request_multipart(
    method: str,
    url: str,
    *,
    fields: dict[str, str],
    file_field: str,
    file_path: Path,
    headers: dict[str, str] | None = None,
    timeout: int = 120,
) -> dict:
    boundary = f"----ifsp-{uuid4().hex}"
    body = multipart_body(fields, file_field, file_path, boundary)
    request = urllib.request.Request(
        url,
        data=body,
        method=method,
        headers={
            **(headers or {}),
            "Content-Type": f"multipart/form-data; boundary={boundary}",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raise RuntimeError(exc.read().decode("utf-8")) from exc


class ApiClient:
    def __init__(self, api_base_url: str, workstation_mac: str, workstation_token: str | None = None) -> None:
        self.api_base_url = api_base_url.rstrip("/")
        self.workstation_mac = workstation_mac
        self.workstation_token = workstation_token

    def register(self, *, ip: str | None = None, hostname: str | None = None) -> dict:
        response = request_json(
            "POST",
            f"{self.api_base_url}/api/fileparse/workstations/register",
            body={
                "mac": self.workstation_mac,
                "ip": ip,
                "hostname": hostname,
                "workstationToken": self.workstation_token,
                "clientVersion": "workstation-cli",
            },
        )
        if not response.get("success"):
            raise RuntimeError(response.get("message") or "register failed")
        data = response["data"]
        if data.get("workstationToken"):
            self.workstation_token = data["workstationToken"]
        return data

    def pull_config(self) -> dict:
        response = request_json(
            "GET",
            f"{self.api_base_url}/api/fileparse/workstations/config?{urllib.parse.urlencode({'mac': self.workstation_mac})}",
            headers=self._auth_headers(),
        )
        if not response.get("success"):
            raise RuntimeError(response.get("message") or "pull config failed")
        return response["data"]

    def push_status(self, data_nos: list[str]) -> dict:
        response = request_json(
            "GET",
            f"{self.api_base_url}/api/fileparse/data/status?{urllib.parse.urlencode({'dataNos': ','.join(data_nos)})}",
            headers=self._auth_headers(),
        )
        if not response.get("success"):
            raise RuntimeError(response.get("message") or "status query failed")
        return response["data"]

    def upload_file(self, *, device_id: int, local_path: Path, file_mtime: datetime) -> dict:
        file_stat = local_path.stat()
        fields = {
            "deviceId": str(device_id),
            "localPath": str(local_path),
            "fileName": local_path.name,
            "fileSize": str(file_stat.st_size),
            "fileMtime": file_mtime.isoformat(),
            "fileHash": sha256_file(local_path),
        }
        payload = request_multipart(
            "POST",
            f"{self.api_base_url}/api/fileparse/files/upload",
            fields=fields,
            file_field="file",
            file_path=local_path,
            headers=self._auth_headers(),
        )
        if not payload.get("success"):
            raise RuntimeError(payload.get("message") or "upload failed")
        return payload["data"]

    def _auth_headers(self) -> dict[str, str]:
        if not self.workstation_token:
            raise RuntimeError("workstationToken is missing")
        return {
            "Authorization": f"Bearer {self.workstation_token}",
            "X-Workstation-Mac": self.workstation_mac,
        }


class SystemApiClient:
    def __init__(self, api_base_url: str, client_id: str, access_token: str, operator: str | None = None) -> None:
        self.api_base_url = api_base_url.rstrip("/")
        self.client_id = client_id
        self.access_token = access_token
        self.operator = operator

    def get_workstation_config(self, workstation_id: int) -> dict:
        response = request_json(
            "GET",
            f"{self.api_base_url}/api/fileparse/workstations/{workstation_id}/config",
            headers=self._auth_headers(),
        )
        if not response.get("success"):
            raise RuntimeError(response.get("message") or "get workstation config failed")
        return response["data"]

    def save_workstation_config(self, workstation_id: int, body: dict) -> dict:
        response = request_json(
            "PUT",
            f"{self.api_base_url}/api/fileparse/workstations/{workstation_id}/config",
            headers=self._auth_headers(),
            body=body,
        )
        if not response.get("success"):
            raise RuntimeError(response.get("message") or "save workstation config failed")
        return response["data"]

    def push_workstation_config(self, workstation_id: int) -> dict:
        response = request_json(
            "POST",
            f"{self.api_base_url}/api/fileparse/workstations/{workstation_id}/config/push",
            headers=self._auth_headers(),
        )
        if not response.get("success"):
            raise RuntimeError(response.get("message") or "push workstation config failed")
        return response["data"]

    def upload_plugin_package(self, file_path: Path) -> dict:
        payload = request_multipart(
            "POST",
            f"{self.api_base_url}/api/fileparse/parsers/upload",
            fields={},
            file_field="file",
            file_path=file_path,
            headers=self._auth_headers(include_operator=True),
        )
        if not payload.get("success"):
            raise RuntimeError(payload.get("message") or "upload plugin failed")
        return payload["data"]

    def list_plugins(self) -> list[dict]:
        response = request_json(
            "GET",
            f"{self.api_base_url}/api/fileparse/parsers",
            headers=self._auth_headers(),
        )
        if not response.get("success"):
            raise RuntimeError(response.get("message") or "list plugins failed")
        return response["data"] or []

    def enable_plugin(self, plugin_name: str) -> dict:
        response = request_json(
            "PUT",
            f"{self.api_base_url}/api/fileparse/parsers/{urllib.parse.quote(plugin_name)}/enable",
            headers=self._auth_headers(),
        )
        if not response.get("success"):
            raise RuntimeError(response.get("message") or "enable plugin failed")
        return response["data"]

    def _auth_headers(self, *, include_operator: bool = False) -> dict[str, str]:
        headers = {
            "Authorization": f"Bearer {self.access_token}",
            "X-Client-Id": self.client_id,
        }
        if include_operator and self.operator:
            headers["X-Operator"] = self.operator
        return headers
