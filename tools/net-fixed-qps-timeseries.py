#!/usr/bin/env python3
"""Fallback fixed-QPS HTTP/1.1 keep-alive time-series sampler.

Use this only when mature fixed-rate tools such as vegeta, hey, or wrk2 are
not available in the current benchmark environment.
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import io
import math
import shutil
import statistics
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlparse


@dataclass
class ServerTarget:
    name: str
    url: str


@dataclass
class Sample:
    second: int
    latency_ms: float
    ok: bool


def percentile(values: list[float], ratio: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(len(ordered) * ratio) - 1))
    return ordered[index]


def parse_server(value: str) -> ServerTarget:
    if "=" not in value:
        raise argparse.ArgumentTypeError("--server must be Name=http://host:port/path")
    name, url = value.split("=", 1)
    if not name or not url:
        raise argparse.ArgumentTypeError("--server requires non-empty name and URL")
    return ServerTarget(name=name, url=url)


async def open_connection(url: str) -> tuple[asyncio.StreamReader, asyncio.StreamWriter, str]:
    parsed = urlparse(url)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or 80
    path = parsed.path or "/"
    if parsed.query:
        path += "?" + parsed.query
    host_header = parsed.netloc or host
    reader, writer = await asyncio.open_connection(host, port)
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host_header}\r\n"
        "Connection: keep-alive\r\n"
        "User-Agent: LikesProgramFixedQps/1.0\r\n"
        "\r\n"
    )
    return reader, writer, request


async def read_http_response(reader: asyncio.StreamReader) -> bool:
    header = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=5.0)
    status_line = header.split(b"\r\n", 1)[0]
    ok = b" 200 " in status_line
    content_length = 0
    for raw_line in header.split(b"\r\n")[1:]:
        if raw_line.lower().startswith(b"content-length:"):
            content_length = int(raw_line.split(b":", 1)[1].strip())
            break
    if content_length > 0:
        await asyncio.wait_for(reader.readexactly(content_length), timeout=5.0)
    return ok


async def worker(
    url: str,
    queue: asyncio.Queue[tuple[int, float] | None],
    samples: list[Sample],
) -> None:
    reader: asyncio.StreamReader | None = None
    writer: asyncio.StreamWriter | None = None
    request = ""
    try:
        while True:
            item = await queue.get()
            if item is None:
                queue.task_done()
                break
            scheduled_second, scheduled_at = item
            try:
                if reader is None or writer is None or writer.is_closing():
                    reader, writer, request = await open_connection(url)
                writer.write(request.encode("ascii"))
                await writer.drain()
                ok = await read_http_response(reader)
                latency_ms = (time.perf_counter() - scheduled_at) * 1000.0
                samples.append(Sample(second=scheduled_second, latency_ms=latency_ms, ok=ok))
            except Exception:
                latency_ms = (time.perf_counter() - scheduled_at) * 1000.0
                samples.append(Sample(second=scheduled_second, latency_ms=latency_ms, ok=False))
                if writer is not None:
                    writer.close()
                    try:
                        await writer.wait_closed()
                    except Exception:
                        pass
                reader = None
                writer = None
            finally:
                queue.task_done()
    finally:
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass


async def run_fixed_qps(
    target: ServerTarget,
    qps: int,
    duration_seconds: int,
    workers: int,
) -> list[Sample]:
    queue_capacity = max(workers * 8, min(qps, 65536), 4096)
    queue: asyncio.Queue[tuple[int, float] | None] = asyncio.Queue(maxsize=queue_capacity)
    samples: list[Sample] = []
    tasks = [
        asyncio.create_task(worker(target.url, queue, samples))
        for _ in range(max(1, workers))
    ]

    start = time.perf_counter()
    tick = 0.05
    carry = 0.0
    steps = max(1, int(duration_seconds / tick))
    for step in range(steps):
        scheduled_at = start + step * tick
        second = min(duration_seconds - 1, int(step * tick))
        carry += qps * tick
        count = int(carry)
        carry -= count
        for _ in range(count):
            try:
                queue.put_nowait((second, scheduled_at))
            except asyncio.QueueFull:
                samples.append(Sample(second=second, latency_ms=0.0, ok=False))
        next_time = start + (step + 1) * tick
        await asyncio.sleep(max(0.0, next_time - time.perf_counter()))

    await queue.join()
    for _ in tasks:
        await queue.put(None)
    await asyncio.gather(*tasks)
    return samples


def resolve_engine(requested: str) -> str:
    if requested == "auto":
        return "hey" if shutil.which("hey") else "python"
    return requested


def safe_name(text: str) -> str:
    return "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in text)


def resolve_hey_shards(workers: int, requested: int, cpu_count: int | None = None) -> int:
    worker_budget = max(1, workers) # 用户给定的总并发预算
    if requested > 0:
        # 显式覆盖仍受总 worker 数约束，避免创建没有并发额度的空分片。
        return min(requested, worker_budget)

    available_cpus = max(1, cpu_count or os.cpu_count() or 1) # 当前运行环境可见 CPU
    cpu_budget = max(1, available_cpus // 2) # 为服务端和系统调度保留 CPU 余量
    concurrency_budget = max(1, worker_budget // 64) # 每个 hey 进程保留足够连接复用并发
    return min(worker_budget, cpu_budget, concurrency_budget)


def run_hey_fixed_qps(
    target: ServerTarget,
    qps: int,
    duration_seconds: int,
    workers: int,
    out_dir: Path,
) -> list[Sample]:
    concurrency = max(1, workers)
    qps_per_worker = qps / concurrency
    log_prefix = out_dir / f"fixed-qps-{qps}-{safe_name(target.name)}.hey"
    hey_path = shutil.which("hey") or "hey"  # Windows 需先解析 PATHEXT 中的 .cmd/.exe
    command = [
        hey_path,
        "-z",
        f"{duration_seconds}s",
        "-c",
        str(concurrency),
        "-q",
        f"{qps_per_worker:.6f}",
        "-o",
        "csv",
        target.url,
    ]

    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    log_prefix.with_suffix(".log").write_text(
        "command=" + " ".join(command) + "\n"
        + f"returncode={completed.returncode}\n"
        + "request_csv_retained=false\n"
        + completed.stderr,
        encoding="utf-8",
    )

    samples: list[Sample] = []
    reader = csv.DictReader(io.StringIO(completed.stdout))
    for row in reader:
        try:
            offset = float(row.get("offset") or 0.0)
            latency_ms = float(row.get("response-time") or 0.0) * 1000.0
            status_code = int(float(row.get("status-code") or 0))
        except (TypeError, ValueError):
            continue

        second = min(duration_seconds - 1, max(0, int(offset)))
        samples.append(Sample(
            second=second,
            latency_ms=latency_ms,
            ok=200 <= status_code < 300,
        ))
    return samples


def summarize(samples: list[Sample], duration_seconds: int) -> list[dict[str, float | int]]:
    buckets: dict[int, list[Sample]] = {}
    for sample in samples:
        buckets.setdefault(sample.second, []).append(sample)

    rows: list[dict[str, float | int]] = []
    for second in range(duration_seconds):
        bucket = buckets.get(second, [])
        ok_latencies = [sample.latency_ms for sample in bucket if sample.ok]
        errors = len([sample for sample in bucket if not sample.ok])
        rows.append({
            "second": second,
            "actual_qps": len(ok_latencies),
            "avg_latency_ms": statistics.fmean(ok_latencies) if ok_latencies else 0.0,
            "p50_latency_ms": percentile(ok_latencies, 0.50),
            "p95_latency_ms": percentile(ok_latencies, 0.95),
            "p99_latency_ms": percentile(ok_latencies, 0.99),
            "errors": errors,
        })
    return rows


def scale(value: float) -> str:
    if value >= 1000:
        return f"{value:,.0f}"
    if value >= 10:
        return f"{value:.1f}"
    return f"{value:.2f}"


def write_latency_svg(
    out_dir: Path,
    qps: int,
    series: dict[str, list[dict[str, float | int]]],
) -> None:
    width = 960
    height = 560
    left = 86
    right = 42
    top = 54
    bottom = 72
    plot_w = width - left - right
    plot_h = height - top - bottom
    all_rows = [row for rows in series.values() for row in rows]
    max_second = max((float(row["second"]) for row in all_rows), default=1.0)
    max_latency = max((float(row["avg_latency_ms"]) for row in all_rows), default=1.0)
    x_max = max(1.0, max_second)
    y_max = max(1.0, max_latency * 1.18)
    colors = {
        "LikesProgramNet": "#0072B2",
        "Nginx": "#D55E00",
    }

    def x_pos(second: float) -> float:
        return left + (second / x_max) * plot_w

    def y_pos(latency: float) -> float:
        return top + plot_h - (latency / y_max) * plot_h

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text x="{left}" y="30" font-family="Arial, sans-serif" font-size="22" font-weight="700" fill="#111827">Fixed QPS {qps}: response time vs duration</text>',
        f'<text x="{left}" y="50" font-family="Arial, sans-serif" font-size="12" fill="#4b5563">x=duration(s), y=mean response time(ms)</text>',
    ]
    for i in range(6):
        y_value = y_max * i / 5
        y = y_pos(y_value)
        parts.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}" stroke="#e5e7eb" stroke-width="1"/>')
        parts.append(f'<text x="{left - 10}" y="{y + 4:.2f}" text-anchor="end" font-family="Arial, sans-serif" font-size="12" fill="#374151">{scale(y_value)}</text>')
    for i in range(6):
        x_value = x_max * i / 5
        x = x_pos(x_value)
        parts.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{top + plot_h}" stroke="#f3f4f6" stroke-width="1"/>')
        parts.append(f'<text x="{x:.2f}" y="{top + plot_h + 24}" text-anchor="middle" font-family="Arial, sans-serif" font-size="12" fill="#374151">{scale(x_value)}</text>')

    parts.append(f'<line x1="{left}" y1="{top + plot_h}" x2="{left + plot_w}" y2="{top + plot_h}" stroke="#111827" stroke-width="1.4"/>')
    parts.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_h}" stroke="#111827" stroke-width="1.4"/>')
    parts.append(f'<text x="{left + plot_w / 2}" y="{height - 20}" text-anchor="middle" font-family="Arial, sans-serif" font-size="15" fill="#111827">Duration (s)</text>')
    parts.append(f'<text x="22" y="{top + plot_h / 2}" text-anchor="middle" font-family="Arial, sans-serif" font-size="15" fill="#111827" transform="rotate(-90 22 {top + plot_h / 2})">Response time (ms)</text>')

    for server, rows in series.items():
        if not rows:
            continue
        color = colors.get(server, "#111827")
        points = " ".join(
            f'{x_pos(float(row["second"])):.2f},{y_pos(float(row["avg_latency_ms"])):.2f}'
            for row in rows
        )
        parts.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="3" stroke-linejoin="round" stroke-linecap="round"/>')

    legend_x = width - 228
    legend_y = 66
    parts.append(f'<rect x="{legend_x - 16}" y="{legend_y - 24}" width="202" height="64" fill="#ffffff" stroke="#d1d5db" rx="4"/>')
    for index, server in enumerate(("LikesProgramNet", "Nginx")):
        y = legend_y + index * 25
        color = colors[server]
        parts.append(f'<line x1="{legend_x}" y1="{y}" x2="{legend_x + 34}" y2="{y}" stroke="{color}" stroke-width="3"/>')
        parts.append(f'<text x="{legend_x + 44}" y="{y + 4}" font-family="Arial, sans-serif" font-size="14" fill="#111827">{server}</text>')
    parts.append("</svg>")
    (out_dir / f"fixed-qps-{qps}-latency-duration.svg").write_text("\n".join(parts), encoding="utf-8")


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--engine", choices=("auto", "hey", "python"), default="auto")
    parser.add_argument("--workers", type=int, default=256)
    parser.add_argument("--qps", nargs="+", type=int, required=True)
    parser.add_argument("--server", action="append", type=parse_server, required=True)
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    engine = resolve_engine(args.engine)
    if engine == "hey" and shutil.which("hey") is None:
        raise SystemExit("fixed QPS engine 'hey' was requested but hey is not available")
    tool_name = "hey-fixed-rate" if engine == "hey" else "python-fixed-rate"
    csv_path = out_dir / "fixed-qps-timeseries.csv"
    summary_path = out_dir / "fixed-qps-summary.csv"

    with csv_path.open("w", encoding="utf-8", newline="") as csv_file, summary_path.open("w", encoding="utf-8", newline="") as summary_file:
        writer = csv.writer(csv_file)
        writer.writerow([
            "target_qps",
            "server",
            "second",
            "actual_qps",
            "avg_latency_ms",
            "p50_latency_ms",
            "p95_latency_ms",
            "p99_latency_ms",
            "errors",
            "tool",
        ])
        summary = csv.writer(summary_file)
        summary.writerow(["target_qps", "server", "avg_actual_qps", "avg_latency_ms", "p95_latency_ms", "p99_latency_ms", "errors", "tool"])

        for qps in args.qps:
            graph_series: dict[str, list[dict[str, float | int]]] = {}
            for server in args.server:
                if engine == "hey":
                    samples = run_hey_fixed_qps(server, qps, args.duration, args.workers, out_dir)
                else:
                    samples = await run_fixed_qps(server, qps, args.duration, args.workers)
                rows = summarize(samples, args.duration)
                graph_series[server.name] = rows
                all_ok = [sample.latency_ms for sample in samples if sample.ok]
                total_errors = len([sample for sample in samples if not sample.ok])
                for row in rows:
                    writer.writerow([
                        qps,
                        server.name,
                        row["second"],
                        row["actual_qps"],
                        f'{row["avg_latency_ms"]:.3f}',
                        f'{row["p50_latency_ms"]:.3f}',
                        f'{row["p95_latency_ms"]:.3f}',
                        f'{row["p99_latency_ms"]:.3f}',
                        row["errors"],
                        tool_name,
                    ])
                summary.writerow([
                    qps,
                    server.name,
                    f'{statistics.fmean(float(row["actual_qps"]) for row in rows):.2f}',
                    f'{statistics.fmean(all_ok) if all_ok else 0.0:.3f}',
                    f'{percentile(all_ok, 0.95):.3f}',
                    f'{percentile(all_ok, 0.99):.3f}',
                    total_errors,
                    tool_name,
                ])
            write_latency_svg(out_dir, qps, graph_series)


if __name__ == "__main__":
    asyncio.run(main())
