#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""无硬件调试入口：假板子跑完「采集 → 分析」，产出与真机完全一样的文件。

不接板子、不占串口、不受人换料影响，用来在 VS Code 里按 F5 走通整条链路：

    py -3.11-32 demo_offline.py --n 20                # 全部成功
    py -3.11-32 demo_offline.py --drop 6              # 演示 C6 掉线 → 重试 → 退出码 3
    py -3.11-32 demo_offline.py --n 10 --no-plots     # 只算表，快

输出（默认在 ``pc/demo/``，已被 .gitignore 忽略）：

    demo/raw.csv            capture 的原始数据（格式与真机一致）
    demo/data/summary.csv   分析结果
    demo/data/compare_theory.csv
    demo/figures/*.png
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
sys.path.insert(0, str(SCRIPT_DIR / "tests"))

import analyze          # noqa: E402  需要先补 sys.path
import capture          # noqa: E402
import fake_board       # noqa: E402


def parse_ids(text: str):
    """``"6"`` / ``"2,6"`` → ``[6]`` / ``[2, 6]``。"""
    text = (text or "").replace("，", ",").strip()
    return [int(part) for part in text.split(",") if part.strip()]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="demo_offline.py",
        description="用假板子把 capture.py + analyze.py 整条链路跑一遍（无需硬件）。",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--outdir", default=str(SCRIPT_DIR / "demo"),
                        help="演示输出目录，raw.csv / data / figures 都放这里")
    parser.add_argument("--cases", default="0-10", help="要测的 case_id")
    parser.add_argument("--n", type=int, default=20, help="每个测点采样数")
    parser.add_argument("--drop", default="", help="故意半路掉线的测点，如 6 或 2,6")
    parser.add_argument("--fail-after", type=int, default=7,
                        help="掉线前先吐几行（验证「已收数据不丢」）")
    parser.add_argument("--error", default="", help="固件直接拒绝的测点，如 5")
    parser.add_argument("--materials", default=str(SCRIPT_DIR / "data" / "materials.csv"),
                        help="材质表")
    parser.add_argument("--no-plots", action="store_true", help="只算表不出图")
    parser.add_argument("--lang", default="zh", choices=("zh", "en"), help="图内语言")
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    outdir = Path(args.outdir).expanduser().resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    raw_path = outdir / "raw.csv"

    board = fake_board.FakeBoard(n=args.n,
                                 drop_cases=parse_ids(args.drop),
                                 fail_after=args.fail_after,
                                 error_cases=parse_ids(args.error))
    cap_args = capture.build_parser().parse_args([
        "--port", "FAKE", "--cases", args.cases, "--n", str(args.n),
        "--interval", "100", "--settle", "0", "--out", str(raw_path),
    ])
    try:
        capture.validate_args(cap_args)
    except capture.CaptureError as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 2

    print(f"[DEMO] 假板子（无串口）→ {raw_path}")
    print(f"[DEMO] 采样数 N={args.n}，掉线测点={sorted(board.drop_cases) or '无'}\n")
    rc = capture.run(cap_args, board)
    if rc != 0:
        print(f"\n[DEMO] 采集按预期中断（rc={rc}）：这正是 --drop/--error 演示的重试路径，"
              f"部分数据已写入 {raw_path}，不再继续分析。")
        return rc

    print("\n[DEMO] 采集完成，开始分析 ……\n")
    analyze_argv = [
        "--input", str(raw_path),
        "--materials", str(Path(args.materials).expanduser()),
        "--outdir", str(outdir / "data"),
        "--figdir", str(outdir / "figures"),
        "--lang", args.lang,
    ]
    if args.no_plots:
        analyze_argv.append("--no-plots")
    rc = analyze.main(analyze_argv)
    if rc == 0:
        print(f"\n[DEMO] 全部产物在 {outdir}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
