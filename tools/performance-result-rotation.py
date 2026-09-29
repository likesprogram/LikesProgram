#!/usr/bin/env python3
"""按显式保留上限轮转 Net/Nginx 压测目录。"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil


RUN_NAME = re.compile(r"^net-nginx-http-\d{8}-\d{6}$")


def rotate_results(out_root: Path, keep_runs: int) -> list[Path]:
    if keep_runs < 0:
        raise ValueError("keep_runs must be at least 0")
    if keep_runs == 0:
        return []
    if not out_root.exists():
        return []

    runs = sorted(
        (path for path in out_root.iterdir() if path.is_dir() and RUN_NAME.fullmatch(path.name)),
        key=lambda path: path.name,
        reverse=True,
    )
    removed = runs[keep_runs:]
    for path in removed:
        shutil.rmtree(path)
    return removed


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-root", type=Path, required=True)
    parser.add_argument("--keep-runs", type=int, default=0)
    args = parser.parse_args()

    removed = rotate_results(args.out_root, args.keep_runs)
    print(f"removed_runs={len(removed)} kept_limit={args.keep_runs}")


if __name__ == "__main__":
    main()
