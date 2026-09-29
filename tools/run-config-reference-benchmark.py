#!/usr/bin/env python3
"""Run Config's mature-parser comparison and preserve reproducible artifacts."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import html
import json
import os
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--label", required=True)
    parser.add_argument("--git-commit", required=True)
    parser.add_argument("--samples", type=int, default=10)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--allow-regressions", action="store_true")
    return parser.parse_args()


def read_rss_kib(pid: int) -> int:
    status = Path(f"/proc/{pid}/status")
    try:
        for line in status.read_text(encoding="ascii").splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except (FileNotFoundError, OSError, ValueError):
        pass
    return 0


def run_once(executable: Path, iterations: int, output: Path) -> tuple[int, float, int, list[dict[str, str]]]:
    started = time.perf_counter()
    with output.open("w", encoding="utf-8", newline="") as stream:
        process = subprocess.Popen(
            [str(executable), "--iterations", str(iterations)],
            stdout=stream,
            stderr=subprocess.STDOUT,
        )
        peak_rss = 0
        while process.poll() is None:
            peak_rss = max(peak_rss, read_rss_kib(process.pid))
            time.sleep(0.005)
        exit_code = process.wait()
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    rows: list[dict[str, str]] = []
    for line in output.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith("metric="):
            continue
        fields = dict(item.split("=", 1) for item in line.split(","))
        fields["peak_rss_kib"] = str(peak_rss)
        rows.append(fields)
    return exit_code, elapsed_ms, peak_rss, rows


def write_svg(path: Path, comparisons: list[dict[str, object]]) -> None:
    width = 1100
    bar_height = 24
    height = max(120, 70 + len(comparisons) * 44)
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        '<text x="24" y="30" font-family="sans-serif" font-size="20" fill="#202124">Config / mature parser mean ns/op ratio</text>',
    ]
    for index, item in enumerate(comparisons):
        y = 52 + index * 44
        ratio = float(item["ratio"])
        length = min(820.0, max(2.0, ratio * 120.0))
        color = "#b3261e" if ratio > 1.0 else "#137333"
        label = html.escape(f'{item["metric"]} vs {item["reference"]}')
        lines.append(f'<text x="24" y="{y + 17}" font-family="monospace" font-size="12">{label}</text>')
        lines.append(f'<rect x="360" y="{y}" width="{length:.1f}" height="{bar_height}" fill="{color}"/>')
        lines.append(f'<text x="{min(360 + length + 8, 1010):.1f}" y="{y + 17}" font-family="monospace" font-size="12">{ratio:.3f}x</text>')
    lines.append("</svg>")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    args = parse_args()
    if args.samples <= 0 or args.warmups < 0 or args.iterations <= 0:
        raise SystemExit("samples and iterations must be positive; warmups cannot be negative")
    executable = args.benchmark.resolve()
    if not executable.is_file():
        raise SystemExit(f"benchmark does not exist: {executable}")

    stamp = dt.datetime.now(dt.timezone.utc).astimezone().strftime("%Y%m%d-%H%M%S")
    output = args.output_root.resolve() / f"config-reference-{args.label}-{stamp}"
    samples_dir = output / "samples"
    warmups_dir = output / "warmups"
    samples_dir.mkdir(parents=True)
    warmups_dir.mkdir()

    warmup_results = []
    for index in range(args.warmups):
        path = warmups_dir / f"warmup-{index + 1:02d}.log"
        code, elapsed, rss, _ = run_once(executable, args.iterations, path)
        warmup_results.append({"index": index + 1, "exit_code": code, "elapsed_ms": elapsed, "peak_rss_kib": rss})
        if code != 0:
            raise SystemExit(f"warmup {index + 1} failed; see {path}")

    all_rows: list[dict[str, str]] = []
    sample_results = []
    for index in range(args.samples):
        path = samples_dir / f"sample-{index + 1:02d}.log"
        code, elapsed, rss, rows = run_once(executable, args.iterations, path)
        sample_results.append({"index": index + 1, "exit_code": code, "elapsed_ms": elapsed, "peak_rss_kib": rss})
        for row in rows:
            row["sample"] = str(index + 1)
            all_rows.append(row)
        if code != 0:
            raise SystemExit(f"sample {index + 1} failed; see {path}")

    raw_csv = output / "measurements.csv"
    fields = ["sample", "metric", "implementation", "format", "operation", "iterations", "total_ns", "ns_per_op", "peak_rss_kib"]
    with raw_csv.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(all_rows)

    grouped: dict[tuple[str, str], list[float]] = {}
    for row in all_rows:
        key = (row["metric"], row["implementation"])
        grouped.setdefault(key, []).append(float(row["ns_per_op"]))
    summaries = []
    for (metric, implementation), values in sorted(grouped.items()):
        summaries.append({
            "metric": metric,
            "implementation": implementation,
            "samples": len(values),
            "mean_ns_per_op": statistics.fmean(values),
            "median_ns_per_op": statistics.median(values),
            "min_ns_per_op": min(values),
            "max_ns_per_op": max(values),
            "stdev_ns_per_op": statistics.stdev(values) if len(values) > 1 else 0.0,
        })
    (output / "summary.json").write_text(json.dumps(summaries, indent=2) + "\n", encoding="utf-8", newline="\n")

    references = {
        "json": ["nlohmann_json", "rapidjson"],
        "yaml": ["yaml_cpp"],
    }
    comparisons: list[dict[str, object]] = []
    for summary in summaries:
        if summary["implementation"] != "likesprogram":
            continue
        matching = [item for item in summaries if item["metric"] == summary["metric"]]
        format_name = "yaml" if summary["metric"].startswith("yaml_") else "json"
        for reference in references[format_name]:
            candidate = next((item for item in matching if item["implementation"] == reference), None)
            if candidate is None:
                continue
            ratio = float(summary["mean_ns_per_op"]) / float(candidate["mean_ns_per_op"])
            comparisons.append({
                "metric": summary["metric"],
                "reference": reference,
                "likesprogram_mean_ns_per_op": summary["mean_ns_per_op"],
                "reference_mean_ns_per_op": candidate["mean_ns_per_op"],
                "ratio": ratio,
                "meets_parity": ratio <= 1.0,
            })
    (output / "comparisons.json").write_text(json.dumps(comparisons, indent=2) + "\n", encoding="utf-8", newline="\n")
    write_svg(output / "ratio.svg", comparisons)

    outcome = "passed" if all(bool(item["meets_parity"]) for item in comparisons) else "failed_performance_parity"
    manifest = {
        "benchmark": str(executable),
        "label": args.label,
        "git_commit": args.git_commit,
        "scope": "same logical documents; native input encoding preconverted; parse and serialize measured separately",
        "host": {
            "os": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "python": sys.version,
            "cpu_count": os.cpu_count(),
        },
        "samples": args.samples,
        "warmups": args.warmups,
        "iterations": args.iterations,
        "warmup_runs": warmup_results,
        "sample_runs": sample_results,
        "outcome": outcome,
        "comparison_count": len(comparisons),
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")

    rows = []
    for item in comparisons:
        status = "PASS" if item["meets_parity"] else "FAIL"
        rows.append("<tr>" + "".join(f"<td>{html.escape(str(item[key]))}</td>" for key in ("metric", "reference", "ratio", "meets_parity")) + f"<td>{status}</td></tr>")
    page = "\n".join([
        "<!doctype html>",
        "<meta charset='utf-8'>",
        f"<title>Config reference benchmark {html.escape(args.label)}</title>",
        f"<h1>Config reference benchmark: {html.escape(args.label)}</h1>",
        f"<p>Outcome: <strong>{html.escape(outcome)}</strong>; commit: <code>{html.escape(args.git_commit)}</code></p>",
        "<p><a href='measurements.csv'>raw measurements</a> | <a href='summary.json'>summary</a> | <a href='comparisons.json'>comparisons</a> | <a href='manifest.json'>manifest</a></p>",
        "<img src='ratio.svg' alt='Config versus mature parser ratio chart'>",
        "<table border='1' cellpadding='6'><thead><tr><th>metric</th><th>reference</th><th>ratio</th><th>parity</th><th>status</th></tr></thead><tbody>",
        *rows,
        "</tbody></table>",
    ])
    (output / "index.html").write_text(page + "\n", encoding="utf-8", newline="\n")
    print(output)
    return 0 if outcome == "passed" or args.allow_regressions else 2


if __name__ == "__main__":
    raise SystemExit(main())
