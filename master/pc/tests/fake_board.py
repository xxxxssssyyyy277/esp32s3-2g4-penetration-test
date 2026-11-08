# -*- coding: utf-8 -*-
"""离线假板子：按 board b 固件的串口行格式应答，用于无硬件自测。

故意在数据中间穿插 ``[DISP]`` 屏幕日志、每 ``empty_every`` 个样本留空一次
rssi、并支持「半路掉线」的测点，用来验证上位机：

* 不会把屏幕日志当成数据行；
* 空 rssi 行仍然保留（记为无效样本）；
* 掉线会触发重试，最终仍失败时已收到的行不丢。

接入方式（见 tests/test_offline.py）::

    board = FakeBoard()
    capture.run(args, board)
"""

from __future__ import annotations

import random
import statistics
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import serial_proto as sp  # noqa: E402  （需要先补 sys.path）

#: 与 board b/main/app_config.h 的 APP_CASE_LIST 同名同序
DEFAULT_CASES: Dict[int, str] = {
    0: "NONE", 1: "PAPER", 2: "BOOK1", 3: "BOOK5", 4: "WATER",
    5: "FOIL", 6: "FOILGAP", 7: "BODY",
    8: "HAND_A1", 9: "HAND_A2", 10: "HAND_A3",
}

#: 各测点相对基准的典型损耗（dB），只用于生成可信的假数据
#: 8–10 号是近场工况（手掌贴住板 A 天线），比 7 号远场人体更大，
#: 且不同人差别明显（手掌大小/干湿/贴合度不同）——假数据也留出这个差异
DEFAULT_LOSSES: Dict[int, float] = {
    0: 0.0, 1: 1.5, 2: 6.0, 3: 9.5, 4: 14.0, 5: 26.0, 6: 21.0, 7: 12.0,
    8: 19.0, 9: 24.0, 10: 21.5,
}


class FakeBoard:
    """实现 ``send`` / ``read_line`` / ``flush_input`` / ``close`` 四个方法的假链路。"""

    def __init__(self, cases: Optional[Dict[int, str]] = None,
                 losses: Optional[Dict[int, float]] = None,
                 n: int = 50, interval_ms: int = 100, base_rssi: int = -45,
                 seed: int = 2024, empty_every: int = 20,
                 drop_cases: Iterable[int] = (), fail_after: int = 7,
                 error_cases: Iterable[int] = ()) -> None:
        self.cases = dict(DEFAULT_CASES if cases is None else cases)
        self.losses = dict(DEFAULT_LOSSES if losses is None else losses)
        self.n = n
        self.interval_ms = interval_ms
        self.base_rssi = base_rssi
        self.seed = seed
        self.empty_every = empty_every
        self.drop_cases = set(drop_cases)
        self.fail_after = fail_after
        self.error_cases = set(error_cases)

        self.outbox: List[str] = []
        self.commands: List[str] = []
        self.closed = False

    # -- 链路接口 ---------------------------------------------------------
    def send(self, command: str) -> None:
        if self.closed:
            raise sp.SerialProtocolError("FakeBoard 已关闭")
        command = command.strip()
        self.commands.append(command)
        self.outbox.extend(self._handle(command))

    def read_line(self, timeout: float) -> str:
        if not self.outbox:
            raise sp.LinkTimeout(f"FakeBoard 没有更多输出（timeout={timeout:.2f} s）")
        return self.outbox.pop(0)

    def flush_input(self) -> None:
        self.outbox.clear()

    def close(self) -> None:
        self.closed = True

    # -- 命令处理 ---------------------------------------------------------
    def _handle(self, command: str) -> List[str]:
        upper = command.upper()
        if upper == "CL":
            lines = ["#BOOT chip=ESP32-S3 role=STA disp=LCD fw=B-1.0.0",
                     f"#CASELIST n={len(self.cases)}"]
            lines.extend(f"{case_id},{label}" for case_id, label in sorted(self.cases.items()))
            lines.extend([f"[DISP] [B]CASELIST n={len(self.cases)}", "#OK CL"])
            return lines
        if upper.startswith("N="):
            text = command[2:]
            if not text.isdigit() or not (sp.N_MIN <= int(text) <= sp.N_MAX):
                return ["#ERR N out of range"]
            self.n = int(text)
            return [f"#OK N n={self.n}"]
        if upper.startswith("I="):
            text = command[2:]
            if not text.isdigit() or not (
                    sp.INTERVAL_MIN_MS <= int(text) <= sp.INTERVAL_MAX_MS):
                return ["#ERR I out of range"]
            self.interval_ms = int(text)
            return [f"#OK I interval_ms={self.interval_ms}"]
        if upper == "P":
            return ["#APINFO ssid=ESP32S3-LINK bssid=AA:BB:CC:DD:EE:FF ch=6 rssi=-45",
                    "[DISP] [B]SSID=ESP32S3-LINK rssi=-45", "#OK P"]
        if upper.startswith("C="):
            text = command[2:]
            if not text.isdigit():
                return ["#ERR bad case id"]
            return self._capture(int(text))
        return [f"#ERR unsupported command {command}"]

    def _capture(self, case_id: int) -> List[str]:
        label = self.cases.get(case_id)
        if label is None:
            return [f"#ERR case_id={case_id} not in table"]
        if case_id in self.error_cases:
            return [f"#ERR C{case_id} disabled"]

        rng = random.Random(self.seed * 1000 + case_id)
        loss = self.losses.get(case_id, 0.0)
        limit = self.n if case_id not in self.drop_cases else min(self.fail_after, self.n)

        lines = [f"#BEGIN case_id={case_id} label={label} n={self.n} "
                 f"interval_ms={self.interval_ms} t0_ms={123000 + case_id}",
                 "case_id,seq,t_ms,rssi_dbm"]
        values: List[int] = []
        for seq in range(1, limit + 1):
            t_ms = (seq - 1) * self.interval_ms
            if self.empty_every and seq % self.empty_every == 0:
                lines.append(f"{case_id},{seq},{t_ms},")
            else:
                rssi = int(round(self.base_rssi - loss + rng.gauss(0.0, 1.5)))
                values.append(rssi)
                lines.append(f"{case_id},{seq},{t_ms},{rssi}")
            if seq % 7 == 0:
                shown = f"{values[-1]}" if values else "?"
                lines.append(f"[DISP] [B]C{case_id} {label} rssi={shown} med=-50.0 "
                             f"base={self.base_rssi}.0 smp={seq}/{self.n}")

        if case_id in self.drop_cases:
            return lines        # 半路掉线：故意不给 #SUMMARY / #END

        if not values:
            return lines + [f"#SUMMARY case_id={case_id} label={label} n={self.n} "
                            "median=0.00 mean=0.00 stdev=0.00 min=0 max=0 valid=0", "#END"]

        median = statistics.median(values)
        mean = statistics.fmean(values)
        stdev = statistics.pstdev(values)
        lines.append(
            f"#SUMMARY case_id={case_id} label={label} n={self.n} "
            f"median={median:.2f} mean={mean:.2f} stdev={stdev:.2f} "
            f"min={min(values)} max={max(values)} valid={len(values)}"
        )
        lines.extend([
            "#END",
            "#OK C",
            f"[DISP] [B]C{case_id} {label} rssi={median:.0f} med={median:.1f} "
            f"base={self.base_rssi}.0 smp={self.n}/{self.n}",
        ])
        return lines