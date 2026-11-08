#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""单板采集：板 B（STA + LCD）遮挡穿透损耗原始数据。

对应 ``提示词-C-上位机.md`` 的「需求一 / capture.py」。核心约束：

* 启动先发 ``CL`` 建立 case_id → label 映射，并打印出来给人确认；
* 采集顺序首尾各插一次基准（默认 case 0）：``0 → 1..N → 0``；
* 每轮之间等 ``--settle`` 秒，留给人插/换遮挡物并退出链路；
* ``[DISP]`` 行丢弃，``#`` 行归档，只有 csv 4 列行进主数据表；
* 解析失败计数并在结束时报告；串口超时重试 3 次，仍失败则保存已采数据后退出。

典型用法（PowerShell，在 pc/ 目录下）::

    python capture.py --port COM5 --cases 0,1,2,3,4,5,6,7,8,9,10 --n 50 --settle 5 --out data/raw.csv
"""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

import serial_proto as sp

SCRIPT_DIR = Path(__file__).resolve().parent

RAW_COLUMNS = [
    "case_id", "label", "seq", "t_ms", "rssi_dbm",
    "round_idx", "round_role", "ap_bssid", "ap_ch",
]
ROLE_FIRST = "first_base"
ROLE_CASE = "case"
ROLE_LAST = "last_base"


class CaptureError(RuntimeError):
    """采集流程错误（可能已经写出了部分数据）。"""


class IncompleteRound(CaptureError):
    """某一轮数据不完整：保留已收到的行，供上层决定是否重试/落盘。"""

    def __init__(self, message: str, rows: Optional[List[dict]] = None,
                 summary: Optional[dict] = None) -> None:
        super().__init__(message)
        self.rows: List[dict] = rows or []
        self.summary = summary


# ---------------------------------------------------------------------------
# 参数与采集顺序
# ---------------------------------------------------------------------------


def parse_cases(text: str) -> List[int]:
    """解析 ``--cases``：支持 ``0,1,2`` 与 ``0-10`` 混写。"""
    text = text.strip().replace("，", ",")
    if not text:
        raise CaptureError("--cases 不能为空，例：--cases 0,1,2,3,4,5,6,7,8,9,10")

    cases: List[int] = []
    for token in text.split(","):
        token = token.strip()
        if not token:
            continue
        if "-" in token:
            start_text, end_text = token.split("-", 1)
            try:
                start, end = int(start_text), int(end_text)
            except ValueError as exc:
                raise CaptureError(f"无法解析 --cases 片段 {token!r}") from exc
            if end < start:
                raise CaptureError(f"--cases 区间 {token!r} 的终点小于起点")
            cases.extend(range(start, end + 1))
        else:
            try:
                cases.append(int(token))
            except ValueError as exc:
                raise CaptureError(f"无法解析 --cases 片段 {token!r}") from exc

    if not cases:
        raise CaptureError("--cases 里没有任何 case_id")
    if any(case < 0 for case in cases):
        raise CaptureError(f"case_id 不能为负：{cases}")
    return cases


def build_sequence(cases: Sequence[int], base_case: int) -> List[Tuple[int, str]]:
    """首尾各插一次基准：``[base] + 非基准测点 + [base]``。"""
    middle = [case for case in cases if case != base_case]
    sequence: List[Tuple[int, str]] = [(base_case, ROLE_FIRST)]
    sequence.extend((case, ROLE_CASE) for case in middle)
    sequence.append((base_case, ROLE_LAST))
    return sequence


# ---------------------------------------------------------------------------
# 单轮采集
# ---------------------------------------------------------------------------


@dataclass
class RoundResult:
    case_id: int
    label: str
    rows: List[dict] = field(default_factory=list)
    summary: Optional[dict] = None
    begin: Optional[dict] = None
    t_begin_host: float = 0.0
    t_end_host: float = 0.0
    attempts: int = 1


def _warn(message: str) -> None:
    print(f"[WARN] {message}")


def measure_once(link, stats: sp.ParseStats, case_id: int, n: int,
                 timeout: float, label: str = "", progress: bool = True) -> RoundResult:
    """发一次 ``C=<id>``，读到 ``#END`` 为止。"""
    link.flush_input()
    link.send(f"C={case_id}")
    deadline = time.monotonic() + timeout

    rows: List[dict] = []
    summary: Optional[dict] = None
    begin: Optional[dict] = None
    t_begin_host = 0.0
    t_end_host = 0.0
    seen_end = False

    while not seen_end:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise IncompleteRound(
                f"C{case_id} {label} 采集超时（{timeout:.0f} s）："
                f"只收到 {len(rows)}/{n} 行。检查板子是否复位、串口是否被 monitor 抢占。",
                rows, summary,
            )
        try:
            line = link.read_line(remaining)
        except sp.LinkTimeout as exc:
            raise IncompleteRound(
                f"C{case_id} {label} 读串口超时：{exc}（已收到 {len(rows)}/{n} 行）",
                rows, summary,
            ) from exc
        kind, payload = sp.classify_line(line)

        if kind == "pair":
            # 采集窗口内出现 2 列行属于异常，按解析失败计数
            stats.note("malformed", line)
        else:
            stats.note(kind, line)

        if line.startswith("#BEGIN"):
            begin = sp.parse_begin(line)
            t_begin_host = time.monotonic()
            if begin["case_id"] != case_id:
                raise IncompleteRound(
                    f"#BEGIN 的 case_id={begin['case_id']} 与请求的 {case_id} 不一致",
                    rows, summary,
                )
        elif line.startswith("#SUMMARY"):
            summary = sp.parse_summary(line)
        elif line.startswith("#END"):
            t_end_host = time.monotonic()
            seen_end = True
        elif line.startswith("#ERR"):
            raise IncompleteRound(f"C{case_id} {label} 被固件拒绝：{line}", rows, summary)
        elif kind == "data":
            assert isinstance(payload, dict)
            if payload["case_id"] != case_id:
                raise IncompleteRound(
                    f"数据行的 case_id={payload['case_id']} 与请求的 {case_id} 不一致：{line!r}",
                    rows, summary,
                )
            rows.append(payload)
            if progress and len(rows) % 10 == 0:
                print(f"\r    C{case_id} 已收 {len(rows)}/{n} 行", end="", flush=True)
        # disp / blank / meta / header / malformed 不需要额外动作：
        # [DISP] 已丢弃，解析失败已在 stats 计数

    if progress and rows:
        print("\r" + " " * 40 + "\r", end="", flush=True)

    if begin is None:
        raise IncompleteRound(f"C{case_id} {label} 没有 #BEGIN，数据区无法定位", rows, summary)
    if begin["n"] != n:
        raise IncompleteRound(
            f"C{case_id} 固件报告 n={begin['n']}，但上位机要求 {n}（N= 命令可能没生效）",
            rows, summary,
        )
    if len(rows) != n:
        raise IncompleteRound(
            f"C{case_id} {label} 只收到 {len(rows)}/{n} 行数据（串口丢行或采集中断）",
            rows, summary,
        )
    if summary is None:
        raise IncompleteRound(f"C{case_id} {label} 没有 #SUMMARY，无法用固件统计交叉校验", rows)

    valid = [row["rssi_dbm"] for row in rows if row["rssi_dbm"] is not None]
    if not valid:
        raise IncompleteRound(
            f"C{case_id} {label} 的 {n} 行数据里没有任何有效 RSSI（板 B 可能掉线）",
            rows, summary,
        )

    # 用 #SUMMARY 交叉校验解析结果（提示词 0.1：汇总行用于校验）
    if summary["case_id"] != case_id:
        raise IncompleteRound(
            f"#SUMMARY 的 case_id={summary['case_id']} 与请求的 {case_id} 不一致",
            rows, summary,
        )
    if summary["valid"] != len(valid):
        _warn(f"C{case_id} {label}：#SUMMARY valid={summary['valid']}，"
              f"上位机解析到 {len(valid)} 行")
    median = float(statistics.median(valid))
    if abs(median - float(summary["median"])) > 0.51:
        _warn(
            f"C{case_id} {label}：上位机中位数 {median:.2f} 与固件 #SUMMARY "
            f"{summary['median']:.2f} 不一致，请检查串口是否有丢行"
        )

    return RoundResult(case_id=case_id, label=label, rows=rows, summary=summary,
                       begin=begin, t_begin_host=t_begin_host, t_end_host=t_end_host)


def measure_with_retry(link, stats: sp.ParseStats, case_id: int, n: int,
                       args, label: str = "") -> RoundResult:
    """失败重试；成功时显式说明丢弃了几行部分数据，不静默。"""
    attempts = max(1, int(args.retries))
    discarded = 0
    last_error: Optional[IncompleteRound] = None

    for attempt in range(1, attempts + 1):
        try:
            result = measure_once(link, stats, case_id, n, args.timeout, label=label)
        except IncompleteRound as exc:
            last_error = exc
            discarded += len(exc.rows)
            _warn(f"第 {attempt}/{attempts} 次尝试失败：{exc}")
            if attempt == attempts:
                # 保留最后一次尝试收到的行，让上层落盘；之前各次的部分行已计在 discarded 里
                exc.discarded = discarded - len(exc.rows)  # type: ignore[attr-defined]
                raise
            pause = min(3.0, max(0.5, float(args.settle) / 2.0))
            print(f"    {pause:.1f} s 后重试（共 {attempts} 次机会）……")
            time.sleep(pause)
            link.flush_input()
            continue

        result.attempts = attempt
        if attempt > 1:
            print(f"    第 {attempt} 次尝试成功；前 {attempt - 1} 次尝试的 "
                  f"{discarded} 行部分数据已丢弃（未写入）")
        return result

    raise last_error  # pragma: no cover - 循环内必然 return 或 raise


# ---------------------------------------------------------------------------
# 统计、节奏与落盘
# ---------------------------------------------------------------------------


def describe_rows(rows: Sequence[dict]) -> dict:
    """按有效样本（rssi 非空）算统计量，与固件 #SUMMARY 口径一致。"""
    values = [int(row["rssi_dbm"]) for row in rows if row["rssi_dbm"] is not None]
    if not values:
        return {"n": len(rows), "valid": 0, "median": None,
                "stdev": None, "min": None, "max": None}
    return {
        "n": len(rows),
        "valid": len(values),
        "median": float(statistics.median(values)),
        "stdev": float(statistics.stdev(values)) if len(values) > 1 else 0.0,
        "min": min(values),
        "max": max(values),
    }


def wait_settle(seconds: float, hint: str) -> None:
    """倒计时，留出插/换遮挡物并退出链路的时间。"""
    seconds = max(0.0, float(seconds))
    if seconds <= 0:
        return
    print(f"\n[{hint}] {seconds:g} s 后开始采集。"
          "请插好遮挡物后立刻退出链路（手留在两块板之间会污染 RSSI）")
    deadline = time.monotonic() + seconds
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        print(f"\r  倒计时 {remaining:4.1f} s", end="", flush=True)
        time.sleep(min(0.2, remaining))
    print("\r" + " " * 24 + "\r", end="", flush=True)


def open_raw_writer(path: Path):
    """打开原始数据 CSV；附带元数据列，方便 analyze.py 直接画图。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = path.open("w", newline="", encoding="utf-8")
    writer = csv.DictWriter(handle, fieldnames=RAW_COLUMNS)
    writer.writeheader()
    handle.flush()
    return handle, writer


def write_round(writer, handle, rows: Sequence[dict], label: str,
                round_idx: int, role: str, ap: Optional[sp.ApInfo]) -> None:
    """把一轮的数据写入主表并立刻 flush（Ctrl+C 也不丢已采数据）。"""
    bssid = ap.bssid if ap is not None else ""
    channel = "" if (ap is None or ap.channel is None) else ap.channel
    for row in rows:
        writer.writerow({
            "case_id": row["case_id"],
            "label": label,
            "seq": row["seq"],
            "t_ms": row["t_ms"],
            "rssi_dbm": "" if row["rssi_dbm"] is None else row["rssi_dbm"],
            "round_idx": round_idx,
            "round_role": role,
            "ap_bssid": bssid,
            "ap_ch": channel,
        })
    handle.flush()


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------


def format_plan(sequence: Sequence[Tuple[int, str]], cases: dict) -> str:
    lines = ["采集顺序（首尾各一次基准，用来判断环境漂移）："]
    for idx, (case_id, role) in enumerate(sequence, 1):
        tag = {ROLE_FIRST: "基准·首", ROLE_LAST: "基准·尾"}.get(role, "测点")
        lines.append(f"  [{idx:>2}/{len(sequence)}] C{case_id} {cases[case_id]:<8} {tag}")
    return "\n".join(lines)


def run(args, link) -> int:
    """执行整轮采集，返回进程退出码（0 = 全部成功）。"""
    stats = sp.ParseStats()
    print("=" * 66)
    print(" 板 B（STA + LCD）遮挡穿透损耗采集 —— capture.py")
    print("=" * 66)
    print(f"串口 {args.port} @ {args.baud}")

    cases = sp.query_caselist(link, stats)
    print("\n固件报的测点表（case_id → label）：")
    for case_id in sorted(cases):
        print(f"  C{case_id}  {cases[case_id]}")
    if isinstance(args.cases, str):      # 直接调用 run() 时可能还没经过 validate_args
        args.cases = parse_cases(args.cases)
    unknown = sorted(set(args.cases) - set(cases))
    if unknown:
        raise CaptureError(
            f"--cases 里的 {unknown} 不在固件测点表中（可用 {sorted(cases)}）；"
            "请先改 board b/main/measure.c 的用例表并重新烧录。"
        )

    sp.set_sample_count(link, stats, args.n)
    if args.interval is not None:
        sp.set_sample_interval(link, stats, args.interval)
    interval_note = (f"，采样间隔 I={args.interval} ms" if args.interval
                     else "（间隔用固件默认 100 ms）")
    print(f"\n采样数 N={args.n}{interval_note}")

    ap = sp.query_apinfo(link, stats)
    print(f"关联的 AP：ssid={ap.ssid or '?'} bssid={ap.bssid or '?'} "
          f"ch={ap.channel if ap.channel is not None else '?'} "
          f"rssi={ap.rssi if ap.rssi is not None else '?'} dBm")
    if not ap.bssid:
        _warn("板 B 的 #APINFO 没有 bssid：可能还没连上板 A 的热点，采集大概率全是空 RSSI")

    sequence = build_sequence(args.cases, args.base)
    print("\n" + format_plan(sequence, cases))

    out_path = Path(args.out).expanduser()
    if not out_path.is_absolute():
        out_path = (Path.cwd() / out_path).resolve()
    handle, writer = open_raw_writer(out_path)
    print(f"\n原始数据 → {out_path}\n")

    rounds: List[Tuple[int, str, RoundResult]] = []
    first_base_median: Optional[float] = None
    exit_code = 0

    try:
        for idx, (case_id, role) in enumerate(sequence, 1):
            label = cases[case_id]
            hint = ("基准：取下全部遮挡物" if role != ROLE_CASE
                    else f"换/插上 C{case_id} {label} 遮挡物")
            wait_settle(args.settle, hint)
            print(f"[{idx}/{len(sequence)}] C{case_id} {label}（{role}）……")

            try:
                result = measure_with_retry(link, stats, case_id, args.n, args, label=label)
            except IncompleteRound as exc:
                print(f"\n[ERROR] {exc}")
                earlier = getattr(exc, "discarded", 0)
                if exc.rows:
                    write_round(writer, handle, exc.rows, label, idx, role, ap)
                    print(f"[WARN] 已把最后一次尝试收到的 {len(exc.rows)} 行写入 {out_path}"
                          "（本轮不完整，analyze.py 会据此告警）")
                if earlier:
                    print(f"[WARN] 更早几次尝试的 {earlier} 行部分数据未写入")
                exit_code = 3
                break

            info = describe_rows(result.rows)
            write_round(writer, handle, result.rows, label, idx, role, ap)
            rounds.append((idx, role, result))
            duration = result.t_end_host - result.t_begin_host
            print(f"    → 有效 {info['valid']}/{info['n']}  中位数 {info['median']:.2f} dBm  "
                  f"标准差 {info['stdev']:.2f} dB  min/max {info['min']}/{info['max']}  "
                  f"用时 {duration:.1f} s（尝试 {result.attempts} 次）")
            if role == ROLE_FIRST:
                first_base_median = info["median"]
            elif role == ROLE_CASE and first_base_median is not None:
                print(f"      相对首次基准损耗 {first_base_median - info['median']:+.2f} dB")
    finally:
        handle.close()

    print("\n" + "-" * 66)
    print(stats.report())
    print("-" * 66)
    if stats.malformed:
        _warn(f"共有 {stats.malformed} 行无法解析（见上方示例），报告里必须如实写明")

    if rounds:
        print("\n本轮汇总：")
        for idx, role, result in rounds:
            info = describe_rows(result.rows)
            print(f"  [{idx:>2}] C{result.case_id:<2} {result.label:<8} {role:<10} "
                  f"median={info['median']:>7.2f} dBm  stdev={info['stdev']:>5.2f} dB  "
                  f"valid={info['valid']}/{info['n']}")

    bases = [(idx, result) for idx, role, result in rounds
             if role in (ROLE_FIRST, ROLE_LAST)]
    if len(bases) == 2:
        first_info = describe_rows(bases[0][1].rows)
        last_info = describe_rows(bases[1][1].rows)
        delta = last_info["median"] - first_info["median"]
        print(f"\n基准漂移检查：首 {first_info['median']:.2f} dBm → "
              f"尾 {last_info['median']:.2f} dBm，Δ = {delta:+.2f} dB")
        if abs(delta) > 3.0:
            print("[WARN] 首尾基准差超过 3 dB：位置/环境/发射功率可能变了，"
                  "这组数据算出的损耗不可信，建议重测")
        else:
            print("  → 在 ±3 dB 以内，可认为环境稳定。")

    if exit_code == 0:
        print(f"\n下一步：python analyze.py --input \"{out_path}\"")
    else:
        print(f"\n[ERROR] 采集未完成；已采数据保留在 {out_path}，没有丢数据。")
    return exit_code


# ---------------------------------------------------------------------------
# 命令行
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="capture.py",
        description="通过串口采集板 B 的多测点 RSSI 扫频数据（首尾各插一次基准）。",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--port", required=True, help="板 B（STA + LCD）的串口，如 COM5")
    parser.add_argument("--baud", type=int, default=115200, help="波特率")
    parser.add_argument("--cases", default="0,1,2,3,4,5,6,7,8,9,10",
                        help="要测的 case_id，支持 0,1,2 与 0-10 混写")
    parser.add_argument("--n", type=int, default=50,
                        help=f"每个测点采样数（{sp.N_MIN}..{sp.N_MAX}）")
    parser.add_argument("--interval", type=int, default=None,
                        help=f"采样间隔 ms（{sp.INTERVAL_MIN_MS}..{sp.INTERVAL_MAX_MS}），"
                             "不给则用固件默认 100 ms")
    parser.add_argument("--settle", type=float, default=5.0,
                        help="每轮开始前留给人换遮挡物的时间（秒）")
    parser.add_argument("--base", type=int, default=0, help="基准测点 case_id（无遮挡）")
    parser.add_argument("--out", default=str(SCRIPT_DIR / "data" / "raw.csv"),
                        help="原始数据 CSV 输出路径")
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="单次尝试的超时秒数（n=50、间隔 100 ms 约需 6 s）")
    parser.add_argument("--retries", type=int, default=3, help="单轮最多尝试次数")
    return parser


def validate_args(args) -> List[int]:
    """命令行参数与固件取值范围的交叉校验，避免板子回 #ERR 才发现。"""
    if not (sp.N_MIN <= args.n <= sp.N_MAX):
        raise CaptureError(f"--n 必须在 {sp.N_MIN}..{sp.N_MAX} 之间，收到 {args.n}")
    if args.interval is not None and not (
            sp.INTERVAL_MIN_MS <= args.interval <= sp.INTERVAL_MAX_MS):
        raise CaptureError(
            f"--interval 必须在 {sp.INTERVAL_MIN_MS}..{sp.INTERVAL_MAX_MS} ms 之间，"
            f"收到 {args.interval}"
        )
    if args.retries < 1:
        raise CaptureError("--retries 至少为 1")
    if args.timeout <= 0:
        raise CaptureError("--timeout 必须为正数")
    if args.settle < 0:
        raise CaptureError("--settle 不能为负")
    if args.base < 0:
        raise CaptureError("--base 不能为负")

    cases = parse_cases(args.cases)
    args.cases = cases
    if args.base not in cases:
        print(f"[INFO] --base {args.base} 不在 --cases 中，仍会在首尾各采一次基准 C{args.base}")
    return cases


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        validate_args(args)
    except CaptureError as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 2

    link = None
    try:
        link = sp.SerialLink(args.port, args.baud, label="B")
        return run(args, link)
    except KeyboardInterrupt:
        print("\n[ABORT] 用户中断（Ctrl+C）；每轮数据都及时 flush，已采部分不会丢。",
              file=sys.stderr)
        return 130
    except (sp.SerialProtocolError, CaptureError) as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 3
    finally:
        if link is not None:
            link.close()


if __name__ == "__main__":
    sys.exit(main())