"""验证性能测试原始记录导出与无效轮次排除规则。"""

import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


RECORD_TOOL = Path(__file__).parents[1] / "performance-test-records.py"
HTTP_BENCHMARK_RUNNER = Path(__file__).parents[1] / "run-net-nginx-http-benchmark.sh"
WRK_RESULT_TOOL = Path(__file__).parents[1] / "net-wrk-result.py"


class PerformanceTestRecordsTests(unittest.TestCase):
    # 资源采样按最低密度验收，不能把 sleep 与采集开销误判为中途缺失。
    def test_http_runner_uses_explicit_minimum_resource_sample_density(self) -> None:
        runner = HTTP_BENCHMARK_RUNNER.read_text(encoding="utf-8")
        self.assertIn('KEEP_RESULT_RUNS="${LP_HTTP_BENCH_KEEP_RUNS:-0}"', runner)
        self.assertIn('RESOURCE_MINIMUM_SAMPLE_PERCENT="${LP_HTTP_RESOURCE_MINIMUM_SAMPLE_PERCENT:-90}"', runner)
        self.assertIn('SEGMENT_SECONDS="${LP_HTTP_BENCH_SEGMENT_SECONDS:-0}"', runner)
        self.assertIn('SEGMENT_CSV_FILE="${RESULT_DIR}/qps-latency-segments.csv"', runner)
        self.assertIn('WRK_RESULT_TOOL="${ROOT_DIR}/tools/net-wrk-result.py"', runner)
        self.assertIn('timeout_error_count', runner)
        self.assertIn('non_success_error_count', runner)
        self.assertIn('LP_HTTP_BENCH_DURATION_SECONDS must be a positive integer', runner)
        self.assertIn('LP_HTTP_BENCH_SEGMENT_SECONDS must be a non-negative integer', runner)
        self.assertIn('segment_started_at="$(date --iso-8601=seconds)"', runner)
        self.assertIn('segment_index,segment_count,offset_seconds,duration_seconds', runner)
        self.assertIn('Segmented QPS/latency data', runner)
        self.assertIn("minimum_sample_count=$((", runner)
        self.assertIn("PLANNED_DURATION_SECONDS * RESOURCE_MINIMUM_SAMPLE_PERCENT", runner)
        self.assertIn('--planned-sample-count "${minimum_sample_count}"', runner)
        self.assertIn(
            'if [[ "${RUN_FINISHED}" == "1" && "${COMPLETED_STAGE_COUNT}" == "${PLANNED_STAGE_COUNT}" ]]; then',
            runner,
        )
        self.assertNotIn(
            'if [[ "${process_status}" == "0" && "${RUN_FINISHED}" == "1"',
            runner,
        )

    # wrk 的 timeout 必须与其他 socket 错误分开，才能定位长稳失败窗口。
    def test_parses_wrk_socket_error_breakdown(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            log_path = Path(temp_dir) / "wrk.log"
            log_path.write_text(
                """Running 30m test @ http://127.0.0.1:18280/
  Latency     6.38ms   28.34ms   1.88s   96.56%
  Latency Distribution
     50%  641.00us
     90%  14.68ms
     99%  79.96ms
  Socket errors: connect 2, read 3, write 4, timeout 115
  Non-2xx or 3xx responses: 7
Requests/sec: 214466.92
""",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [sys.executable, str(WRK_RESULT_TOOL), str(log_path)],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(
                completed.stdout.strip(),
                "214466.92 6.380 0.641 14.680 79.960 131 2 3 4 115 7",
            )

    # 完整失败轮次仍是有效测试数据，中断或缺文件轮次不是。
    def test_exports_completed_pass_and_fail_records_without_rewriting_raw_values(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)  # 隔离四类输入与生成产物
            records_root = root / "records"  # 模拟性能结果根目录
            output_root = root / "dashboard"  # 保存规范化导出
            records_root.mkdir()

            self._write_record(records_root, "passed", True, "passed", "123.456789")
            self._write_record(records_root, "failed", True, "failed", "9.876543")
            self._write_record(records_root, "interrupted", False, "failed", "7.000000")
            self._write_record(records_root, "missing-raw", True, "failed", None)

            completed = subprocess.run(
                [
                    sys.executable,
                    str(RECORD_TOOL),
                    "--records-root",
                    str(records_root),
                    "--output-dir",
                    str(output_root),
                ],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            records = json.loads((output_root / "test-records.json").read_text(encoding="utf-8"))
            self.assertEqual([record["record_id"] for record in records], ["failed", "passed"])
            self.assertEqual([record["outcome"] for record in records], ["failed", "passed"])

            samples = json.loads((output_root / "test-samples.json").read_text(encoding="utf-8"))
            raw_values = {sample["record_id"]: sample["fields"]["throughput"] for sample in samples}
            self.assertEqual(raw_values, {"failed": "9.876543", "passed": "123.456789"})

            rejected = json.loads((output_root / "rejected-records.json").read_text(encoding="utf-8"))
            self.assertEqual(
                {record["record_id"] for record in rejected},
                {"interrupted", "missing-raw"},
            )

    # CSV 导出只封装字段，不改写原始 CSV 中的值。
    def test_exports_record_and_sample_csv_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)  # 隔离单轮失败结果
            records_root = root / "records"  # 原始记录根目录
            output_root = root / "dashboard"  # CSV 导出目录
            records_root.mkdir()
            self._write_record(records_root, "failed", True, "failed", "9.876543")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(RECORD_TOOL),
                    "--records-root",
                    str(records_root),
                    "--output-dir",
                    str(output_root),
                ],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            with (output_root / "test-records.csv").open("r", encoding="utf-8", newline="") as handle:
                records = list(csv.DictReader(handle))
            self.assertEqual(records[0]["record_id"], "failed")
            self.assertEqual(records[0]["outcome"], "failed")

            with (output_root / "test-samples.csv").open("r", encoding="utf-8", newline="") as handle:
                samples = list(csv.DictReader(handle))
            fields = json.loads(samples[0]["fields_json"])
            self.assertEqual(fields["throughput"], "9.876543")

    # 总 HTML 必须离线包含全部有效轮次、原始值与必要筛选器。
    def test_writes_self_contained_dashboard_for_passed_and_failed_records(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)  # 隔离看板夹具
            records_root = root / "records"  # 两个完整轮次
            output_root = root / "dashboard"  # 总看板输出目录
            records_root.mkdir()
            self._write_record(records_root, "passed", True, "passed", "123.456789")
            self._write_record(records_root, "failed", True, "failed", "9.876543")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(RECORD_TOOL),
                    "--records-root",
                    str(records_root),
                    "--output-dir",
                    str(output_root),
                ],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            dashboard = (output_root / "index.html").read_text(encoding="utf-8")
            self.assertIn("原始性能测试总看板", dashboard)
            self.assertIn('id="version-filter"', dashboard)
            self.assertIn('id="test-type-filter"', dashboard)
            self.assertIn('id="platform-filter"', dashboard)
            self.assertIn('id="backend-filter"', dashboard)
            self.assertIn('id="outcome-filter"', dashboard)
            self.assertIn('id="records-data"', dashboard)
            self.assertIn('id="samples-data"', dashboard)
            self.assertIn("123.456789", dashboard)
            self.assertIn("9.876543", dashboard)
            self.assertIn("test-records.csv", dashboard)
            self.assertIn("test-samples.csv", dashboard)
            self.assertIn("@media (max-width: 720px)", dashboard)
            self.assertIn('rel="icon" href="data:image/gif;base64,', dashboard)
            self.assertIn("location.protocol !== 'file:'", dashboard)

    # raw_files 可以包含日志，只有 tabular_files 指定的 CSV 才展开为原始行。
    def test_validates_raw_files_but_only_parses_tabular_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)  # 隔离混合原始文件
            records_root = root / "records"  # 单轮结果目录
            output_root = root / "dashboard"  # 规范化导出目录
            record_root = records_root / "mixed"  # 同时包含日志与 CSV
            record_root.mkdir(parents=True)
            (record_root / "run.log").write_text("line one\nline two\n", encoding="utf-8")
            (record_root / "metrics.csv").write_text(
                "throughput,error_count\n12.500000,4\n",
                encoding="utf-8",
            )
            manifest = {
                "schema_version": 1,
                "record_id": "mixed",
                "completed": True,
                "outcome": "failed",
                "version": "87267e3",
                "test_type": "proxy_relay",
                "platform": "multi-platform",
                "backend": "multiple",
                "started_at": "2026-07-16T14:00:00+08:00",
                "finished_at": "2026-07-16T14:30:00+08:00",
                "raw_globs": ["*.log", "*.csv"],
                "tabular_globs": ["*.csv"],
            }
            (record_root / "test-record.json").write_text(
                json.dumps(manifest, ensure_ascii=False),
                encoding="utf-8",
            )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(RECORD_TOOL),
                    "--records-root",
                    str(records_root),
                    "--output-dir",
                    str(output_root),
                ],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            samples = json.loads((output_root / "test-samples.json").read_text(encoding="utf-8"))
            self.assertEqual([sample["source_file"] for sample in samples], ["metrics.csv"])

    # v2 记录按阶段、时长和采样证明完整执行；非零退出码只保留原始结果事实。
    def test_schema_two_requires_complete_execution_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)  # 隔离五种执行证据
            records_root = root / "records"  # 保存待校验轮次
            output_root = root / "dashboard"  # 保存拒绝诊断
            records_root.mkdir()
            variants = {
                "valid": {"exit_code": 0, "planned_stage_count": 2, "completed_stage_count": 2,
                          "planned_duration_seconds": 60, "observed_duration_seconds": 61,
                          "planned_sample_count": 60, "completed_sample_count": 60},
                "complete-failed": {"exit_code": 2, "planned_stage_count": 2, "completed_stage_count": 2,
                                    "planned_duration_seconds": 60, "observed_duration_seconds": 61,
                                    "planned_sample_count": 60, "completed_sample_count": 60},
                "missing-stage": {"exit_code": 0, "planned_stage_count": 2, "completed_stage_count": 1,
                                  "planned_duration_seconds": 60, "observed_duration_seconds": 61,
                                  "planned_sample_count": 60, "completed_sample_count": 60},
                "short-run": {"exit_code": 0, "planned_stage_count": 2, "completed_stage_count": 2,
                              "planned_duration_seconds": 60, "observed_duration_seconds": 59,
                              "planned_sample_count": 60, "completed_sample_count": 60},
                "short-sampling": {"exit_code": 0, "planned_stage_count": 2, "completed_stage_count": 2,
                                   "planned_duration_seconds": 60, "observed_duration_seconds": 61,
                                   "planned_sample_count": 60, "completed_sample_count": 59},
            }  # valid 与 complete-failed 都满足完整执行约束
            for record_id, execution in variants.items():
                self._write_record(records_root, record_id, True, "failed", "1.000000")
                self._update_manifest(
                    records_root / record_id / "test-record.json",
                    {"schema_version": 2, "execution": execution},
                )

            completed = self._run_export(records_root, output_root)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            records = json.loads((output_root / "test-records.json").read_text(encoding="utf-8"))
            self.assertEqual(
                [record["record_id"] for record in records],
                ["complete-failed", "valid"],
            )
            self.assertEqual(records[0]["execution"]["exit_code"], 2)
            rejected = json.loads((output_root / "rejected-records.json").read_text(encoding="utf-8"))
            self.assertEqual(
                {record["record_id"] for record in rejected},
                {"missing-stage", "short-run", "short-sampling"},
            )

    # v2 只接受明确结果枚举和正向、带时区的执行时间范围。
    def test_schema_two_rejects_invalid_outcome_and_time_range(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)  # 隔离损坏的 manifest 元数据
            records_root = root / "records"  # 两个伪完整轮次
            output_root = root / "dashboard"  # 保存拒绝诊断
            records_root.mkdir()
            execution = {
                "exit_code": 0,
                "planned_stage_count": 1,
                "completed_stage_count": 1,
                "planned_duration_seconds": 1,
                "observed_duration_seconds": 2,
                "planned_sample_count": 1,
                "completed_sample_count": 1,
            }  # 执行证据本身完整，错误只来自元数据
            self._write_record(records_root, "bad-outcome", True, "cancelled", "1.000000")
            self._update_manifest(
                records_root / "bad-outcome" / "test-record.json",
                {"schema_version": 2, "execution": execution},
            )
            self._write_record(records_root, "bad-time", True, "failed", "1.000000")
            self._update_manifest(
                records_root / "bad-time" / "test-record.json",
                {
                    "schema_version": 2,
                    "execution": execution,
                    "started_at": "2026-07-16T15:30:00+08:00",
                    "finished_at": "2026-07-16T15:00:00+08:00",
                },
            )

            completed = self._run_export(records_root, output_root)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            records = json.loads((output_root / "test-records.json").read_text(encoding="utf-8"))
            self.assertEqual(records, [])
            rejected = json.loads((output_root / "rejected-records.json").read_text(encoding="utf-8"))
            self.assertEqual({record["record_id"] for record in rejected}, {"bad-outcome", "bad-time"})

    # 声明为必需的 glob 未命中文件时，整轮不能进入正式记录。
    def test_rejects_required_glob_without_matches(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)  # 隔离 glob 缺失场景
            records_root = root / "records"  # 单轮完整声明
            output_root = root / "dashboard"  # 保存拒绝原因
            records_root.mkdir()
            self._write_record(records_root, "missing-log", True, "failed", "1.000000")
            self._update_manifest(
                records_root / "missing-log" / "test-record.json",
                {"raw_globs": ["metrics.csv", "*.log"], "raw_files": []},
            )

            completed = self._run_export(records_root, output_root)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            records = json.loads((output_root / "test-records.json").read_text(encoding="utf-8"))
            self.assertEqual(records, [])
            rejected = json.loads((output_root / "rejected-records.json").read_text(encoding="utf-8"))
            self.assertIn("matched no files", rejected[0]["reason"])

    # 重复轮次标识会混合原始行，因此所有冲突记录都必须拒绝。
    def test_rejects_all_records_with_duplicate_record_id(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)  # 隔离重复标识
            records_root = root / "records"  # 两个不同目录
            output_root = root / "dashboard"  # 保存冲突诊断
            records_root.mkdir()
            self._write_record(records_root, "first", True, "passed", "1.000000")
            self._write_record(records_root, "second", True, "failed", "2.000000")
            self._update_manifest(records_root / "first" / "test-record.json", {"record_id": "duplicate"})
            self._update_manifest(records_root / "second" / "test-record.json", {"record_id": "duplicate"})

            completed = self._run_export(records_root, output_root)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            records = json.loads((output_root / "test-records.json").read_text(encoding="utf-8"))
            self.assertEqual(records, [])
            rejected = json.loads((output_root / "rejected-records.json").read_text(encoding="utf-8"))
            self.assertEqual(len(rejected), 2)
            self.assertTrue(all("duplicate record_id" in record["reason"] for record in rejected))

    # 输出目录可以与记录根目录分离，原始文件链接仍须可用。
    def test_dashboard_links_raw_files_relative_to_output_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)  # 记录和看板位于兄弟目录
            records_root = root / "records"  # 原始证据根目录
            output_root = root / "dashboard"  # HTML 输出目录
            records_root.mkdir()
            self._write_record(records_root, "passed", True, "passed", "1.000000")

            completed = self._run_export(records_root, output_root)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            dashboard = (output_root / "index.html").read_text(encoding="utf-8")
            self.assertIn('"raw_href_path":"../records/passed"', dashboard)

    # 不同时区的轮次必须按绝对完成时刻排序，不能按 ISO 文本字面排序。
    def test_orders_records_by_absolute_finished_time(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)  # 隔离两个跨时区轮次
            records_root = root / "records"  # 保存待排序轮次
            output_root = root / "dashboard"  # 保存导出结果
            records_root.mkdir()
            self._write_record(records_root, "a-older-local", True, "passed", "1.000000")
            self._update_manifest(
                records_root / "a-older-local" / "test-record.json",
                {"finished_at": "2026-07-16T13:00:00+08:00"},
            )
            self._write_record(records_root, "z-newer-remote", True, "passed", "2.000000")
            self._update_manifest(
                records_root / "z-newer-remote" / "test-record.json",
                {"finished_at": "2026-07-16T01:00:00-07:00"},
            )

            completed = self._run_export(records_root, output_root)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            records = json.loads((output_root / "test-records.json").read_text(encoding="utf-8"))
            self.assertEqual(
                [record["record_id"] for record in records],
                ["z-newer-remote", "a-older-local"],
            )

    # 运行器通过同一工具原子写入 v2 manifest，避免手工美化测试结果。
    def test_writes_schema_two_manifest_from_execution_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            result_root = Path(temp_dir) / "result"  # 模拟单轮结果目录
            manifest_path = result_root / "test-record.json"  # 正式 manifest 路径

            completed = subprocess.run(
                [
                    sys.executable,
                    str(RECORD_TOOL),
                    "--write-manifest",
                    str(manifest_path),
                    "--record-id",
                    "long-failed",
                    "--completed",
                    "true",
                    "--outcome",
                    "failed",
                    "--version",
                    "abc123",
                    "--test-type",
                    "http_long_stability_30m",
                    "--platform",
                    "linux-6.8",
                    "--backend",
                    "io_uring",
                    "--started-at",
                    "2026-07-16T15:00:00+08:00",
                    "--finished-at",
                    "2026-07-16T15:31:00+08:00",
                    "--exit-code",
                    "0",
                    "--planned-stage-count",
                    "2",
                    "--completed-stage-count",
                    "2",
                    "--planned-duration-seconds",
                    "1800",
                    "--observed-duration-seconds",
                    "1860",
                    "--planned-sample-count",
                    "1800",
                    "--completed-sample-count",
                    "1860",
                    "--raw-glob",
                    "*.log",
                    "--raw-glob",
                    "*.csv",
                    "--tabular-glob",
                    "*.csv",
                ],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(manifest["schema_version"], 2)
            self.assertTrue(manifest["completed"])
            self.assertEqual(manifest["outcome"], "failed")
            self.assertEqual(manifest["execution"]["completed_stage_count"], 2)
            self.assertEqual(manifest["execution"]["completed_sample_count"], 1860)
            self.assertEqual(manifest["raw_globs"], ["*.log", "*.csv"])
            self.assertEqual(manifest["tabular_globs"], ["*.csv"])
            self.assertFalse(manifest_path.with_suffix(".json.tmp").exists())

    # 执行导出工具并保留 stdout/stderr 供失败断言使用。
    def _run_export(self, records_root: Path, output_root: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(RECORD_TOOL),
                "--records-root",
                str(records_root),
                "--output-dir",
                str(output_root),
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    # 合并测试专用 manifest 字段，避免夹具重复序列化细节。
    def _update_manifest(self, manifest_path: Path, updates: dict[str, object]) -> None:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest.update(updates)
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")

    # 创建一个最小规范记录；原始 CSV 只有在 raw_value 非空时落盘。
    def _write_record(
        self,
        records_root: Path,
        record_id: str,
        completed: bool,
        outcome: str,
        raw_value: str | None,
    ) -> None:
        record_root = records_root / record_id  # 每轮结果拥有独立目录
        record_root.mkdir()
        manifest = {
            "schema_version": 1,
            "record_id": record_id,
            "completed": completed,
            "outcome": outcome,
            "version": "87267e3",
            "test_type": "http_long_stability",
            "platform": "linux-6.8",
            "backend": "io_uring-multishot-provided-buffer",
            "started_at": "2026-07-16T14:00:00+08:00",
            "finished_at": "2026-07-16T14:30:00+08:00",
            "raw_files": ["metrics.csv"],
        }
        (record_root / "test-record.json").write_text(
            json.dumps(manifest, ensure_ascii=False),
            encoding="utf-8",
        )
        if raw_value is None:
            return
        with (record_root / "metrics.csv").open("w", encoding="utf-8", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(["throughput", "error_count"])
            writer.writerow([raw_value, "0" if outcome == "passed" else "3"])


if __name__ == "__main__":
    unittest.main()
