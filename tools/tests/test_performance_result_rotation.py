"""验证完整压测结果目录按最近轮次自动轮转。"""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROTATION_TOOL = Path(__file__).parents[1] / "performance-result-rotation.py"


class PerformanceResultRotationTests(unittest.TestCase):
    def test_zero_disables_automatic_rotation(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            for stamp in ("001000", "002000", "003000", "004000"):
                (root / f"net-nginx-http-20260710-{stamp}").mkdir()

            completed = subprocess.run(
                [sys.executable, str(ROTATION_TOOL), "--out-root", str(root), "--keep-runs", "0"],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(len(list(root.iterdir())), 4)
            self.assertIn("removed_runs=0 kept_limit=0", completed.stdout)

    def test_keeps_only_latest_matching_runs(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            for stamp in ("001000", "002000", "003000", "004000"):
                (root / f"net-nginx-http-20260710-{stamp}").mkdir()
            unrelated = root / "manual-notes"
            unrelated.mkdir()

            completed = subprocess.run(
                [sys.executable, str(ROTATION_TOOL), "--out-root", str(root), "--keep-runs", "2"],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            remaining = sorted(path.name for path in root.iterdir())
            self.assertEqual(
                remaining,
                [
                    "manual-notes",
                    "net-nginx-http-20260710-003000",
                    "net-nginx-http-20260710-004000",
                ],
            )


if __name__ == "__main__":
    unittest.main()
