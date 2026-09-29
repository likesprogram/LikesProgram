#!/usr/bin/env python3
"""Parse one wrk result into stable numeric fields for benchmark CSV files."""

from __future__ import annotations

import re
import sys
from pathlib import Path


SOCKET_ERROR_PATTERN = re.compile(
    r"Socket errors:\s*connect\s+([0-9]+),\s*read\s+([0-9]+),\s*"
    r"write\s+([0-9]+),\s*timeout\s+([0-9]+)"
)
NON_SUCCESS_PATTERN = re.compile(r"Non-2xx or 3xx responses:\s*([0-9]+)")


def to_ms(value: str, unit: str) -> float:
    number = float(value)
    if unit == "us":
        return number / 1000.0
    if unit == "s":
        return number * 1000.0
    return number


def parse(text: str) -> tuple[float, float, float, float, float, int, int, int, int, int, int]:
    qps_match = re.search(r"Requests/sec:\s*([0-9.]+)", text)
    latency_match = re.search(r"Latency\s+([0-9.]+)\s*(us|ms|s)", text)
    if qps_match is None or latency_match is None:
        raise ValueError("failed to parse wrk output")

    def percentile(label: str) -> float:
        match = re.search(rf"^\s*{label}%\s+([0-9.]+)\s*(us|ms|s)", text, re.MULTILINE)
        if match is None:
            return 0.0
        return to_ms(match.group(1), match.group(2))

    socket_errors = SOCKET_ERROR_PATTERN.search(text)
    connect_errors = int(socket_errors.group(1)) if socket_errors else 0
    read_errors = int(socket_errors.group(2)) if socket_errors else 0
    write_errors = int(socket_errors.group(3)) if socket_errors else 0
    timeout_errors = int(socket_errors.group(4)) if socket_errors else 0
    non_success_match = NON_SUCCESS_PATTERN.search(text)
    non_success_errors = int(non_success_match.group(1)) if non_success_match else 0
    error_count = (
        connect_errors
        + read_errors
        + write_errors
        + timeout_errors
        + non_success_errors
    )
    return (
        float(qps_match.group(1)),
        to_ms(latency_match.group(1), latency_match.group(2)),
        percentile("50"),
        percentile("90"),
        percentile("99"),
        error_count,
        connect_errors,
        read_errors,
        write_errors,
        timeout_errors,
        non_success_errors,
    )


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} WRK_LOG", file=sys.stderr)
        return 2
    try:
        values = parse(Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace"))
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(
        f"{values[0]:.2f} {values[1]:.3f} {values[2]:.3f} {values[3]:.3f} "
        f"{values[4]:.3f} {values[5]} {values[6]} {values[7]} {values[8]} "
        f"{values[9]} {values[10]}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
