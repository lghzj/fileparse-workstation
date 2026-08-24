import argparse
import asyncio
import json
import math
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from statistics import mean

from workstation.client import ApiClient, request_multipart, sha256_file
from workstation.config import WorkstationConfig, load_config


@dataclass
class UploadResult:
    index: int
    data_no: str
    file_name: str
    upload_seconds: float
    total_seconds: float | None = None
    final_status: str | None = None
    error_message: str | None = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Workstation upload/parse load test")
    parser.add_argument("--server", default="python/workstation/server.json")
    parser.add_argument("--file", required=True)
    parser.add_argument("--device-id", type=int, default=None)
    parser.add_argument("--total", type=int, default=20)
    parser.add_argument("--concurrency", type=int, default=4)
    parser.add_argument("--timeout-seconds", type=int, default=120)
    parser.add_argument("--poll-interval", type=float, default=0.5)
    parser.add_argument("--prefix", default="loadtest")
    return parser.parse_args()


def build_client(config: WorkstationConfig) -> ApiClient:
    return ApiClient(config.api_base_url, config.mac, config.workstation_token)


def upload_one(
    *,
    client: ApiClient,
    source_file: Path,
    device_id: int,
    index: int,
    prefix: str,
) -> UploadResult:
    started = time.monotonic()
    timestamp = datetime.now().strftime("%Y%m%d%H%M%S")
    file_name = f"{prefix}_{timestamp}_{index:04d}{source_file.suffix}"
    local_path = f"/pressure/{file_name}"
    file_stat = source_file.stat()
    file_hash = sha256_file(source_file)
    payload = request_multipart(
        "POST",
        f"{client.api_base_url}/api/fileparse/files/upload",
        fields={
            "deviceId": str(device_id),
            "localPath": local_path,
            "fileName": file_name,
            "fileSize": str(file_stat.st_size),
            "fileMtime": datetime.fromtimestamp(file_stat.st_mtime).isoformat(),
            "fileHash": file_hash,
        },
        file_field="file",
        file_path=source_file,
        headers=client._auth_headers(),
        timeout=120,
    )
    if not payload.get("success"):
        raise RuntimeError(payload.get("message") or "upload failed")
    data = payload["data"]
    return UploadResult(
        index=index,
        data_no=data["dataNo"],
        file_name=file_name,
        upload_seconds=time.monotonic() - started,
    )


async def run_load_test(args: argparse.Namespace) -> dict:
    config = load_config(Path(args.server))
    if not config.workstation_token:
        raise RuntimeError("workstationToken is missing in server config")
    device_id = args.device_id or _default_device_id(config)
    source_file = Path(args.file)
    if not source_file.is_file():
        raise RuntimeError(f"sample file not found: {source_file}")

    client = build_client(config)
    started = time.monotonic()
    semaphore = asyncio.Semaphore(args.concurrency)
    results: list[UploadResult] = []

    async def upload_task(index: int) -> None:
        async with semaphore:
            result = await asyncio.to_thread(
                upload_one,
                client=client,
                source_file=source_file,
                device_id=device_id,
                index=index,
                prefix=args.prefix,
            )
            results.append(result)

    await asyncio.gather(*(upload_task(index) for index in range(1, args.total + 1)))
    by_no = {item.data_no: item for item in results}

    deadline = time.monotonic() + args.timeout_seconds
    pending = set(by_no)
    while pending and time.monotonic() < deadline:
        statuses = await asyncio.to_thread(client.push_status, sorted(pending))
        for item in statuses:
            data_no = item["dataNo"]
            status = item["status"]
            if status not in {"success", "failed"}:
                continue
            result = by_no[data_no]
            result.final_status = status
            result.total_seconds = time.monotonic() - started
            result.error_message = item.get("errorMessage")
            pending.discard(data_no)
        if pending:
            await asyncio.sleep(args.poll_interval)

    for data_no in pending:
        by_no[data_no].final_status = "timeout"
        by_no[data_no].total_seconds = None

    return build_summary(
        results=results,
        started=started,
        device_id=device_id,
        total=args.total,
        concurrency=args.concurrency,
        source_file=source_file,
    )


def _default_device_id(config: WorkstationConfig) -> int:
    enabled_items = [item for item in config.items if item.get("enabled")]
    if not enabled_items:
        raise RuntimeError("no enabled device binding found in server config")
    return int(enabled_items[0]["deviceId"])


def percentile(values: list[float], ratio: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * ratio) - 1))
    return ordered[index]


def build_summary(
    *,
    results: list[UploadResult],
    started: float,
    device_id: int,
    total: int,
    concurrency: int,
    source_file: Path,
) -> dict:
    upload_latencies = [item.upload_seconds for item in results]
    parse_latencies = [item.total_seconds for item in results if item.total_seconds is not None]
    counts: dict[str, int] = {}
    for item in results:
        counts[item.final_status or "unknown"] = counts.get(item.final_status or "unknown", 0) + 1
    finished = sum(counts.get(key, 0) for key in ("success", "failed"))
    wall_seconds = time.monotonic() - started
    return {
        "deviceId": device_id,
        "sourceFile": str(source_file),
        "total": total,
        "concurrency": concurrency,
        "wallSeconds": round(wall_seconds, 3),
        "completed": finished,
        "counts": counts,
        "uploadLatency": summarize_latencies(upload_latencies),
        "endToEndLatency": summarize_latencies(parse_latencies),
        "results": [
            {
                "index": item.index,
                "dataNo": item.data_no,
                "fileName": item.file_name,
                "uploadSeconds": round(item.upload_seconds, 3),
                "totalSeconds": round(item.total_seconds, 3) if item.total_seconds is not None else None,
                "status": item.final_status,
                "errorMessage": item.error_message,
            }
            for item in sorted(results, key=lambda row: row.index)
        ],
    }


def summarize_latencies(values: list[float]) -> dict:
    if not values:
        return {"count": 0}
    return {
        "count": len(values),
        "avg": round(mean(values), 3),
        "p50": round(percentile(values, 0.50), 3),
        "p90": round(percentile(values, 0.90), 3),
        "p95": round(percentile(values, 0.95), 3),
        "max": round(max(values), 3),
    }


def main() -> None:
    args = parse_args()
    summary = asyncio.run(run_load_test(args))
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
