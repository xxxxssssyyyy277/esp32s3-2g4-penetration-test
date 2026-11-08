# -*- coding: utf-8 -*-
"""analyze.py 的离线自测：用 FakeBoard 生成 raw.csv，再端到端跑分析。

需要 pandas/numpy/matplotlib；没装时整类跳过，``unittest discover`` 仍然全绿。
"""

from __future__ import annotations

import csv
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

PC_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PC_DIR))
sys.path.insert(0, str(PC_DIR / "tests"))

import analyze                      # noqa: E402
import capture                      # noqa: E402
import fake_board                   # noqa: E402

HAS_STACK = all(importlib.util.find_spec(name) is not None
                for name in ("pandas", "numpy", "matplotlib"))


def write_demo_raw(raw_path: Path, n: int = 50) -> None:
    """不碰串口，用 FakeBoard 跑一遍采集拿到真实格式的 raw.csv。"""
    args = capture.build_parser().parse_args([
        "--port", "FAKE", "--cases", "0-10", "--n", str(n),
        "--interval", "50", "--settle", "0", "--out", str(raw_path),
    ])
    capture.validate_args(args)
    rc = capture.run(args, fake_board.FakeBoard(n=n))
    if rc != 0:
        raise AssertionError(f"假板采集失败，rc={rc}")


def read_csv_rows(path: Path):
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


@unittest.skipUnless(HAS_STACK, "需要 pandas + numpy + matplotlib（pip install -r requirements.txt）")
class AnalyzeTest(unittest.TestCase):

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.raw = self.tmp / "raw.csv"
        self.materials = self.tmp / "materials.csv"
        self.materials.write_text(
            (PC_DIR / "data" / "materials.csv").read_text(encoding="utf-8-sig"),
            encoding="utf-8",
        )
        self.outdir = self.tmp / "data"
        self.figdir = self.tmp / "figures"
        write_demo_raw(self.raw)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_analyze(self, *extra: str) -> int:
        return analyze.main([
            "--input", str(self.raw),
            "--materials", str(self.materials),
            "--outdir", str(self.outdir),
            "--figdir", str(self.figdir),
            "--date", "2024-01-01",
            *extra,
        ])

    def test_summary_and_figures(self):
        """正常数据：10 个测点、损耗排序正确、三张图和两个 CSV 都生成。"""
        self.assertEqual(self.run_analyze(), 0)

        summary = read_csv_rows(self.outdir / "summary.csv")
        self.assertEqual(len(summary), 10)
        self.assertEqual([row["case_id"] for row in summary],
                         ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10"])

        by_label = {row["label"]: float(row["loss_db"]) for row in summary}
        self.assertAlmostEqual(by_label["FOIL"], 26.0, delta=1.5)
        self.assertAlmostEqual(by_label["PAPER"], 1.0, delta=1.5)
        self.assertAlmostEqual(by_label["HAND_A1"], 19.0, delta=1.5)
        self.assertAlmostEqual(by_label["HAND_A2"], 24.0, delta=1.5)
        self.assertAlmostEqual(by_label["HAND_A3"], 21.5, delta=1.5)
        # 手捂板 A 是近场工况，损耗应明显大于远场人体
        for label in ("HAND_A1", "HAND_A2", "HAND_A3"):
            self.assertGreater(by_label[label], by_label["BODY"], label)
        # 不同人捂住板 A 的反应差别必须体现出来（这里取 3 人极差）
        hands = [by_label["HAND_A1"], by_label["HAND_A2"], by_label["HAND_A3"]]
        self.assertGreaterEqual(max(hands) - min(hands), 3.0)
        self.assertEqual(max(by_label, key=by_label.get), "FOIL")
        self.assertEqual(min(by_label, key=by_label.get), "PAPER")

        compare = read_csv_rows(self.outdir / "compare_theory.csv")
        self.assertEqual(len(compare), 10)
        verdicts = {row["label"]: row["verdict"] for row in compare}
        # 近场手掌遮挡超出 water 类的远场经验量级（3–15 dB），必须判「超出」
        for label in ("HAND_A1", "HAND_A2", "HAND_A3"):
            self.assertEqual(verdicts[label], "超出", label)
        for label, verdict in verdicts.items():
            if not label.startswith("HAND_"):
                self.assertEqual(verdict, "符合", label)

        for name in ("loss_by_material.png", "baseline_drift.png",
                     "loss_by_thickness.png"):
            self.assertTrue((self.figdir / name).is_file(), name)

    def test_baseline_drift_refuses_to_plot(self):
        """首尾基准差 > 3 dB：退出码 3，且不出任何表/图。"""
        rows = read_csv_rows(self.raw)
        with self.raw.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            for row in rows:
                if row["round_role"] == "last_base" and row["rssi_dbm"]:
                    row["rssi_dbm"] = f"{float(row['rssi_dbm']) + 6.0:.0f}"
                writer.writerow(row)

        self.assertEqual(self.run_analyze(), 3)
        self.assertFalse((self.outdir / "summary.csv").exists())
        self.assertFalse(self.figdir.exists())

    def test_no_plots_keeps_csv_only(self):
        """--no-plots：只写 CSV，不建 figures 目录。"""
        self.assertEqual(self.run_analyze("--no-plots"), 0)
        self.assertTrue((self.outdir / "summary.csv").is_file())
        self.assertFalse(self.figdir.exists())

    def test_missing_materials_reports_reason(self):
        """材质表缺失时必须报错，不能靠猜。"""
        self.materials.unlink()
        self.assertEqual(self.run_analyze(), 3)


if __name__ == "__main__":
    unittest.main()
