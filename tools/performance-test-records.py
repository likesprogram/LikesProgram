#!/usr/bin/env python3
"""收集完整性能测试轮次，并原样导出记录与 CSV 行。"""

import argparse
from collections import Counter
import csv
from datetime import datetime, timezone
import json
import os
from pathlib import Path
from typing import Any


# 读取并校验单轮 manifest 的基础结构。
def load_manifest(manifest_path: Path) -> dict[str, Any]:
    with manifest_path.open("r", encoding="utf-8") as handle:
        value = json.load(handle)  # 保留 manifest 中的原始字段
    if not isinstance(value, dict):
        raise ValueError("manifest root must be an object")
    return value


# 把拒绝原因收敛为稳定诊断记录，不混入测试数据。
def reject_record(
    rejected: list[dict[str, str]],
    manifest_path: Path,
    record_id: str,
    reason: str,
) -> None:
    rejected.append(
        {
            "record_id": record_id,
            "manifest_path": manifest_path.as_posix(),
            "reason": reason,
        }
    )


# 展开 manifest 中的相对 glob，并返回稳定、去重的文件列表。
def expand_files(record_root: Path, patterns: Any) -> list[str]:
    if patterns is None:
        return []
    if not isinstance(patterns, list):
        raise ValueError("file globs must be a list")
    expanded: set[str] = set()  # 多个 glob 可能命中同一文件
    for pattern in patterns:
        matches = [path for path in record_root.glob(str(pattern)) if path.is_file()]  # 当前必需模式的实际文件
        if not matches:
            raise ValueError(f"file glob matched no files: {pattern}")
        for path in matches:
            if path.is_file():
                expanded.add(path.relative_to(record_root).as_posix())
    return sorted(expanded)


# v2 manifest 必须用机器可校验字段证明计划完整执行。
def validate_execution(manifest: dict[str, Any]) -> str:
    if manifest.get("schema_version") != 2:
        return ""
    if manifest.get("outcome") not in {"passed", "failed"}:
        return f"schema v2 outcome is invalid: {manifest.get('outcome')}"
    try:
        started_at = datetime.fromisoformat(str(manifest.get("started_at")))  # 带时区开始时间
        finished_at = datetime.fromisoformat(str(manifest.get("finished_at")))  # 带时区结束时间
    except ValueError:
        return "schema v2 timestamps must use ISO 8601"
    if started_at.utcoffset() is None or finished_at.utcoffset() is None:
        return "schema v2 timestamps must include timezone offsets"
    if finished_at < started_at:
        return "schema v2 finished_at precedes started_at"
    execution = manifest.get("execution")  # 运行器在正常或异常退出时写入的证据
    if not isinstance(execution, dict):
        return "schema v2 execution must be an object"

    exit_code = execution.get("exit_code")  # 原样保留程序退出码，不据此美化筛选结果
    planned_stages = execution.get("planned_stage_count")  # 计划测量阶段数
    completed_stages = execution.get("completed_stage_count")  # 实际完整测量阶段数
    planned_duration = execution.get("planned_duration_seconds")  # 计划测量总秒数
    observed_duration = execution.get("observed_duration_seconds")  # 实际墙钟秒数
    planned_samples = execution.get("planned_sample_count")  # 计划资源采样行数
    completed_samples = execution.get("completed_sample_count")  # 实际资源采样行数
    values = [
        exit_code,
        planned_stages,
        completed_stages,
        planned_duration,
        observed_duration,
        planned_samples,
        completed_samples,
    ]
    if any(isinstance(value, bool) or not isinstance(value, (int, float)) for value in values):
        return "schema v2 execution fields must be numeric"
    if planned_stages <= 0 or completed_stages != planned_stages:
        return f"test stages incomplete: {completed_stages}/{planned_stages}"
    if planned_duration < 0 or observed_duration < planned_duration:
        return f"test duration incomplete: {observed_duration}/{planned_duration} seconds"
    if planned_samples < 0 or completed_samples < planned_samples:
        return f"test sampling incomplete: {completed_samples}/{planned_samples} rows"
    return ""


# 把带时区完成时间统一到 UTC；历史缺失值稳定排到最后。
def finished_at_sort_value(record: dict[str, Any]) -> datetime:
    try:
        finished_at = datetime.fromisoformat(str(record.get("finished_at")))  # 保留原文，只生成排序键
    except ValueError:
        return datetime.min.replace(tzinfo=timezone.utc)
    if finished_at.utcoffset() is None:
        return datetime.min.replace(tzinfo=timezone.utc)
    return finished_at.astimezone(timezone.utc)


# 收集已完整执行且原始文件齐全的轮次，失败结果仍照常保留。
def collect_records(
    records_root: Path,
    output_dir: Path | None = None,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, str]]]:
    records: list[dict[str, Any]] = []  # 完整测试轮次
    samples: list[dict[str, Any]] = []  # 原始 CSV 数据行
    rejected: list[dict[str, str]] = []  # 无效轮次诊断
    root_resolved = records_root.resolve()  # 约束 raw_files 不能越界
    manifests: list[tuple[Path, dict[str, Any], str]] = []  # 先加载以检测跨目录重复 ID

    for manifest_path in sorted(records_root.rglob("test-record.json")):
        try:
            manifest = load_manifest(manifest_path)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            reject_record(rejected, manifest_path, manifest_path.parent.name, str(error))
            continue

        record_id = str(manifest.get("record_id") or manifest_path.parent.name)  # 稳定轮次标识
        manifests.append((manifest_path, manifest, record_id))

    id_counts = Counter(record_id for _, _, record_id in manifests)  # 重复 ID 会混合详情数据
    for manifest_path, manifest, record_id in manifests:
        if id_counts[record_id] > 1:
            reject_record(rejected, manifest_path, record_id, f"duplicate record_id: {record_id}")
            continue
        if manifest.get("completed") is not True:
            reject_record(rejected, manifest_path, record_id, "test did not complete")
            continue
        execution_error = validate_execution(manifest)  # v1 历史记录维持兼容，v2 严格校验
        if execution_error:
            reject_record(rejected, manifest_path, record_id, execution_error)
            continue

        raw_files = manifest.get("raw_files", [])  # 可显式登记固定原始文件
        if not isinstance(raw_files, list):
            reject_record(rejected, manifest_path, record_id, "raw_files must be a list")
            continue
        try:
            globbed_raw = expand_files(manifest_path.parent, manifest.get("raw_globs"))
            globbed_tabular = expand_files(manifest_path.parent, manifest.get("tabular_globs"))
        except ValueError as error:
            reject_record(rejected, manifest_path, record_id, str(error))
            continue

        raw_names = sorted({str(raw_name) for raw_name in raw_files} | set(globbed_raw))
        if not raw_names:
            reject_record(rejected, manifest_path, record_id, "raw_files must not be empty")
            continue

        tabular_files = manifest.get("tabular_files")  # 显式 CSV 子集
        if tabular_files is None:
            tabular_files = globbed_tabular if manifest.get("tabular_globs") is not None else raw_names
        if not isinstance(tabular_files, list):
            reject_record(rejected, manifest_path, record_id, "tabular_files must be a list")
            continue
        tabular_files = sorted({str(raw_name) for raw_name in tabular_files} | set(globbed_tabular))

        invalid_reason = ""  # 任一原始文件异常即拒绝整轮
        for raw_name in raw_names:
            raw_path = (manifest_path.parent / raw_name).resolve()  # 当前轮原始文件
            if not raw_path.is_relative_to(root_resolved):
                invalid_reason = f"raw file escapes records root: {raw_name}"
                break
            if not raw_path.is_file():
                invalid_reason = f"raw file is missing: {raw_name}"
                break
        if invalid_reason:
            reject_record(rejected, manifest_path, record_id, invalid_reason)
            continue

        record_samples: list[dict[str, Any]] = []  # 整轮通过校验后才并入总数据
        for raw_name in tabular_files:
            raw_name = str(raw_name)  # CSV 相对路径
            if raw_name not in raw_names:
                invalid_reason = f"tabular file is not listed in raw_files: {raw_name}"
                break
            raw_path = (manifest_path.parent / raw_name).resolve()  # 待展开 CSV
            try:
                with raw_path.open("r", encoding="utf-8", newline="") as handle:
                    reader = csv.DictReader(handle)  # 不转换、不舍入任何原始值
                    if reader.fieldnames is None:
                        invalid_reason = f"raw CSV has no header: {raw_name}"
                        break
                    for row_number, row in enumerate(reader, start=2):
                        record_samples.append(
                            {
                                "record_id": record_id,
                                "source_file": str(raw_name),
                                "row_number": row_number,
                                "fields": dict(row),
                            }
                        )
            except (OSError, UnicodeError, csv.Error) as error:
                invalid_reason = f"raw CSV cannot be read: {raw_name}: {error}"
                break

        if invalid_reason:
            reject_record(rejected, manifest_path, record_id, invalid_reason)
            continue

        record = dict(manifest)  # 原始 manifest 字段完整进入导出
        record["record_id"] = record_id
        record["record_path"] = manifest_path.parent.relative_to(records_root).as_posix()
        record["raw_href_path"] = (
            Path(os.path.relpath(manifest_path.parent, output_dir)).as_posix()
            if output_dir is not None
            else record["record_path"]
        )  # 看板可与记录根目录分离
        record["raw_files"] = raw_names  # glob 在导出中固化为本轮实际文件
        record["tabular_files"] = tabular_files
        records.append(record)
        samples.extend(record_samples)

    records.sort(key=lambda record: str(record["record_id"]))  # 同一完成时刻按 ID 稳定升序
    records.sort(key=finished_at_sort_value, reverse=True)  # 绝对完成时刻由新到旧
    samples.sort(key=lambda sample: (str(sample["record_id"]), str(sample["source_file"]), int(sample["row_number"])))
    rejected.sort(key=lambda record: str(record["record_id"]))
    return records, samples, rejected


# 以 UTF-8、稳定缩进写出可直接审计的 JSON。
def write_json(path: Path, value: Any) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


# 把复杂 manifest 字段封装为 JSON，标量文本保持原值。
def csv_value(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (dict, list)):
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    return str(value)


# 导出一轮一行的记录表，附加字段按名称稳定排序。
def write_records_csv(path: Path, records: list[dict[str, Any]]) -> None:
    preferred = [
        "record_id",
        "completed",
        "outcome",
        "version",
        "test_type",
        "platform",
        "backend",
        "started_at",
        "finished_at",
        "record_path",
        "raw_files",
    ]  # 首屏常用列保持固定顺序
    extra = sorted({key for record in records for key in record} - set(preferred))
    fieldnames = preferred + extra  # 未知 manifest 字段仍完整导出
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for record in records:
            writer.writerow({key: csv_value(record.get(key)) for key in fieldnames})


# 导出逐原始行索引；fields_json 内保留每个原始文本值。
def write_samples_csv(path: Path, samples: list[dict[str, Any]]) -> None:
    fieldnames = ["record_id", "source_file", "row_number", "fields_json"]
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for sample in samples:
            writer.writerow(
                {
                    "record_id": sample["record_id"],
                    "source_file": sample["source_file"],
                    "row_number": sample["row_number"],
                    "fields_json": json.dumps(
                        sample["fields"],
                        ensure_ascii=False,
                        separators=(",", ":"),
                    ),
                }
            )


# 把 JSON 安全嵌入 script 节点，避免记录文本提前闭合标签。
def embedded_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")


# 使用仓库模板生成可直接从 file:// 打开的总看板。
def write_dashboard(
    path: Path,
    records: list[dict[str, Any]],
    samples: list[dict[str, Any]],
    rejected_count: int,
) -> None:
    template_path = Path(__file__).with_name("templates") / "performance-test-dashboard.html"
    template = template_path.read_text(encoding="utf-8")  # 静态布局与交互模板
    generated_at = datetime.now(timezone.utc).isoformat()  # 记录生成快照时间
    html = template.replace("__RECORDS_DATA__", embedded_json(records))
    html = html.replace("__SAMPLES_DATA__", embedded_json(samples))
    html = html.replace("__GENERATED_AT__", generated_at)
    html = html.replace("__REJECTED_COUNT__", str(rejected_count))
    path.write_text(html, encoding="utf-8")


# 原子写入运行器生成的 v2 manifest，避免半写文件被总看板扫描。
def write_manifest(path: Path, args: argparse.Namespace) -> None:
    manifest = {
        "schema_version": 2,
        "record_id": args.record_id,
        "completed": args.completed == "true",
        "outcome": args.outcome,
        "version": args.version,
        "test_type": args.test_type,
        "platform": args.platform,
        "backend": args.backend,
        "started_at": args.started_at,
        "finished_at": args.finished_at,
        "raw_globs": args.raw_glob,
        "tabular_globs": args.tabular_glob,
        "execution": {
            "exit_code": args.exit_code,
            "planned_stage_count": args.planned_stage_count,
            "completed_stage_count": args.completed_stage_count,
            "planned_duration_seconds": args.planned_duration_seconds,
            "observed_duration_seconds": args.observed_duration_seconds,
            "planned_sample_count": args.planned_sample_count,
            "completed_sample_count": args.completed_sample_count,
        },
    }  # 所有字段来自运行时事实，不派生性能结论
    if args.completion_evidence:
        manifest["completion_evidence"] = args.completion_evidence
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = path.with_suffix(path.suffix + ".tmp")  # 同目录替换保持原子性
    write_json(temporary_path, manifest)
    temporary_path.replace(path)


# 校验写入 manifest 模式的必需参数，避免生成模糊记录。
def require_manifest_arguments(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    required = [
        "record_id",
        "completed",
        "outcome",
        "version",
        "test_type",
        "platform",
        "backend",
        "started_at",
        "finished_at",
        "exit_code",
        "planned_stage_count",
        "completed_stage_count",
        "planned_duration_seconds",
        "observed_duration_seconds",
        "planned_sample_count",
        "completed_sample_count",
    ]  # argparse 的共享模式无法直接逐项 required
    missing = [name for name in required if getattr(args, name) is None]
    if missing:
        parser.error("manifest mode missing arguments: " + ", ".join(missing))
    if not args.raw_glob:
        parser.error("manifest mode requires at least one --raw-glob")


# 解析命令行并写入单轮 manifest 或生成原始记录导出。
def main() -> int:
    parser = argparse.ArgumentParser(description="Export completed raw performance test records")
    parser.add_argument("--records-root", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--write-manifest", type=Path)
    parser.add_argument("--record-id")
    parser.add_argument("--completed", choices=["true", "false"])
    parser.add_argument("--outcome", choices=["passed", "failed"])
    parser.add_argument("--version")
    parser.add_argument("--test-type")
    parser.add_argument("--platform")
    parser.add_argument("--backend")
    parser.add_argument("--started-at")
    parser.add_argument("--finished-at")
    parser.add_argument("--exit-code", type=int)
    parser.add_argument("--planned-stage-count", type=int)
    parser.add_argument("--completed-stage-count", type=int)
    parser.add_argument("--planned-duration-seconds", type=int)
    parser.add_argument("--observed-duration-seconds", type=int)
    parser.add_argument("--planned-sample-count", type=int)
    parser.add_argument("--completed-sample-count", type=int)
    parser.add_argument("--raw-glob", action="append", default=[])
    parser.add_argument("--tabular-glob", action="append", default=[])
    parser.add_argument("--completion-evidence")
    args = parser.parse_args()

    if args.write_manifest is not None:
        require_manifest_arguments(parser, args)
        write_manifest(args.write_manifest.resolve(), args)
        print(f"manifest={args.write_manifest}")
        return 0
    if args.records_root is None or args.output_dir is None:
        parser.error("export mode requires --records-root and --output-dir")

    records_root = args.records_root.resolve()  # 输入结果根目录
    output_dir = args.output_dir.resolve()  # 生成文件目录
    output_dir.mkdir(parents=True, exist_ok=True)
    records, samples, rejected = collect_records(records_root, output_dir)
    write_json(output_dir / "test-records.json", records)
    write_json(output_dir / "test-samples.json", samples)
    write_json(output_dir / "rejected-records.json", rejected)
    write_records_csv(output_dir / "test-records.csv", records)
    write_samples_csv(output_dir / "test-samples.csv", samples)
    write_dashboard(output_dir / "index.html", records, samples, len(rejected))
    print(f"records={len(records)} samples={len(samples)} rejected={len(rejected)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
