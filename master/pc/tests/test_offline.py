# -*- coding: utf-8 -*-
"""离线自测：不需要真实硬件，用 FakeBoard 跑通 capture.py 全流程。

运行（在 master/ 下）::

    python -m unittest discover -s pc/tests -v
"""

from __future__ import annotations

import csv
import sys
import tempfile
import unittest
from pathlib import Path

PC_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PC_DIR))
sys.path.insert(0, str(PC_DIR / "tests"))

import capture  # noqa: E402
import fake_board  # noqa: E402
import serial_proto as sp  # noqa: E402


def make_args(output_dir: Path, **overrides) -> object:
    argv = [
        "--port", "FAKE", "--cases", "0,1,2,3,4,5,6,7,8,9,10",
        "--n", "20", "--settle", "0", "--timeout", "5", "--retries", "2",
        "--out", str(output_dir / "raw.csv"),
    ]
    args = capture.build_parser().parse_args(argv)
    capture.validate_args(args)
    for key, value in overrides.items():
        setattr(args, key, value)
    return args


def read_csv(path: Path):
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


class SequenceTest(unittest.TestCase):
    def test_parse_cases(self):
        self.assertEqual(capture.parse_cases("0-2,5"), [0, 1, 2, 5])
        self.assertEqual(capture.parse_cases("0，1"), [0, 1])
        with self.assertRaises(capture.CaptureError):
            capture.parse_cases("3-1")
        with self.assertRaises(capture.CaptureError):
            capture.parse_cases("")

    def test_sequence_has_base_at_both_ends(self):
        self.assertEqual(
            capture.build_sequence([0, 1, 2], 0),
            [(0, "first_base"), (1, "case"), (2, "case"), (0, "last_base")],
        )
        # --cases 里没有基准时也要在首尾补上
        self.assertEqual(
            capture.build_sequence([1, 2], 0),
            [(0, "first_base"), (1, "case"), (2, "case"), (0, "last_base")],
        )


class FullCaptureTest(unittest.TestCase):
    def test_offline_run_writes_all_rounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = make_args(Path(tmp))
            board = fake_board.FakeBoard(empty_every=20)
            rc = capture.run(args, board)

            self.assertEqual(rc, 0)
            out = Path(tmp) / "raw.csv"
            rows = read_csv(out)
            self.assertEqual(list(rows[0].keys()), capture.RAW_COLUMNS)
            # 11 个测点，其中 case 0 只作基准：首 0 + 1..10 + 尾 0 = 12 轮
            self.assertEqual(len(rows), 12 * 20)

            roles = [row["round_role"] for row in rows]
            self.assertEqual(roles[0], "first_base")
            self.assertEqual(roles[-1], "last_base")
            self.assertEqual(roles.count("case"), 10 * 20)

            # 每轮 seq=20 的 rssi 由 FakeBoard 留空，必须保留成无效样本
            invalid = [row for row in rows if row["rssi_dbm"] == ""]
            self.assertEqual(len(invalid), 12)

            self.assertEqual({row["label"] for row in rows},
                             set(fake_board.DEFAULT_CASES.values()))
            self.assertEqual({row["ap_bssid"] for row in rows}, {"aa:bb:cc:dd:ee:ff"})
            self.assertEqual({row["ap_ch"] for row in rows}, {"6"})

            # 命令顺序：CL → N= → P → C=0 ……，基准共采两次
            self.assertEqual(board.commands[:3], ["CL", "N=20", "P"])
            self.assertEqual(board.commands.count("C=0"), 2)

    def test_dropped_case_keeps_partial_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = make_args(Path(tmp))
            board = fake_board.FakeBoard(empty_every=0, drop_cases={3}, fail_after=5)
            rc = capture.run(args, board)

            self.assertEqual(rc, 3)                        # 明确失败退出
            rows = read_csv(Path(tmp) / "raw.csv")
            partial = [row for row in rows if row["case_id"] == "3"]
            self.assertEqual(len(partial), 5)              # 已收到的行没丢
            self.assertTrue(all(row["round_role"] == "case" for row in partial))
            self.assertEqual(len([row for row in rows if row["case_id"] == "0"]), 20)
            self.assertEqual(board.commands.count("C=3"), 2)   # 重试过 1 次

    def test_unknown_case_fails_before_measuring(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = make_args(Path(tmp), cases=[0, 42])
            board = fake_board.FakeBoard()
            with self.assertRaises(capture.CaptureError):
                capture.run(args, board)
            self.assertFalse(any(cmd.startswith("C=") for cmd in board.commands))


class MeasureOnceTest(unittest.TestCase):
    def test_disp_lines_are_dropped(self):
        board = fake_board.FakeBoard(n=20, empty_every=0)
        stats = sp.ParseStats()
        result = capture.measure_once(board, stats, 0, 20, 5.0,
                                      label="NONE", progress=False)

        self.assertEqual(len(result.rows), 20)
        self.assertEqual(stats.data_rows, 20)
        self.assertGreater(stats.disp_dropped, 0)
        self.assertEqual(stats.malformed, 0)
        self.assertEqual(result.summary["case_id"], 0)
        self.assertEqual(result.begin["n"], 20)

    def test_malformed_line_is_counted(self):
        board = fake_board.FakeBoard(n=20, empty_every=0)
        original_send = board.send

        def send_with_junk(command: str) -> None:
            original_send(command)
            if command.startswith("C="):
                board.outbox.insert(3, "this-is-not-csv")

        board.send = send_with_junk  # type: ignore[assignment]
        stats = sp.ParseStats()
        result = capture.measure_once(board, stats, 1, 20, 5.0,
                                      label="PAPER", progress=False)

        self.assertEqual(len(result.rows), 20)
        self.assertEqual(stats.malformed, 1)
        self.assertEqual(stats.malformed_samples, ["this-is-not-csv"])

    def test_wrong_n_is_rejected(self):
        board = fake_board.FakeBoard(n=20, empty_every=0)
        stats = sp.ParseStats()
        with self.assertRaises(capture.IncompleteRound):
            capture.measure_once(board, stats, 0, 50, 5.0, progress=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)