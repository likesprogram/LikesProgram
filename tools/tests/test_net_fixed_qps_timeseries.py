"""验证固定 QPS 工具不会默认保留逐请求大文件。"""

import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest


MODULE_PATH = Path(__file__).parents[1] / "net-fixed-qps-timeseries.py"
MODULE_SPEC = importlib.util.spec_from_file_location("net_fixed_qps_timeseries", MODULE_PATH)
MODULE = importlib.util.module_from_spec(MODULE_SPEC)
assert MODULE_SPEC.loader is not None
sys.modules[MODULE_SPEC.name] = MODULE
MODULE_SPEC.loader.exec_module(MODULE)


class FixedQpsArtifactTests(unittest.TestCase):
    def test_hey_auto_shards_follow_cpu_and_worker_budget(self) -> None:
        resolver = getattr(MODULE, "resolve_hey_shards", None)
        self.assertIsNotNone(resolver, "hey shard resolver should exist")
        self.assertEqual(resolver(256, 0, cpu_count=2), 1)
        self.assertEqual(resolver(256, 0, cpu_count=8), 4)
        self.assertEqual(resolver(32, 0, cpu_count=16), 1)
        self.assertEqual(resolver(256, 3, cpu_count=2), 3)

    def test_hey_discards_request_csv_by_default(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            hey_path = fake_bin / ("hey.cmd" if os.name == "nt" else "hey")
            if os.name == "nt":
                hey_path.write_text(
                    "@echo offset,response-time,status-code\r\n"
                    "@echo 0.1,0.002,200\r\n",
                    encoding="utf-8",
                )
            else:
                hey_path.write_text(
                    "#!/usr/bin/env sh\n"
                    "printf 'offset,response-time,status-code\\n0.1,0.002,200\\n'\n",
                    encoding="utf-8",
                )
                hey_path.chmod(0o755)

            old_path = os.environ.get("PATH", "")
            os.environ["PATH"] = str(fake_bin) + os.pathsep + old_path
            try:
                samples = MODULE.run_hey_fixed_qps(
                    MODULE.ServerTarget("LikesProgramNet", "http://127.0.0.1:18080/"),
                    5000,
                    1,
                    1,
                    root,
                )
            finally:
                os.environ["PATH"] = old_path

            self.assertEqual(len(samples), 1)
            self.assertFalse((root / "fixed-qps-5000-LikesProgramNet.csv").exists())
            self.assertTrue((root / "fixed-qps-5000-LikesProgramNet.log").exists())

if __name__ == "__main__":
    unittest.main()
