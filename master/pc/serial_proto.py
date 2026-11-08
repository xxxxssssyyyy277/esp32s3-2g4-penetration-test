#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""板 A / 板 B 串口行协议解析（上位机共用模块）。

本模块只依赖标准库；pyserial 在 :class:`SerialLink` 里延迟导入，
因此没装 pyserial 也能跑解析层的离线自测。

协议硬规则（对应 ``提示词-C-上位机.md`` 0.1 节）：

1. ``[DISP]`` 开头的屏幕同步日志必须直接丢弃，绝不参与数值解析；
2. ``#`` 开头的行按前缀分别处理，不进入主数据表；
3. 只有能被 csv 模块按 4 列解析的行才进主数据表；
4. 解析失败的行必须计数并在结束时报告，不许静默跳过。

实际固件（board a / board b）的输出格式::

    #BOOT chip=ESP32-S3 role=STA disp=LCD fw=B-1.0.0
    #APINFO ssid=ESP32S3-LINK bssid=aa:bb:cc:dd:ee:ff ch=6 rssi=-45
    #CASELIST n=11
    0,NONE
    ...
    #OK CL
    #BEGIN case_id=6 label=FOILGAP n=50 interval_ms=100 t0_ms=123456
    case_id,seq,t_ms,rssi_dbm
    6,1,0,-58
    6,2,100,          <- 采集失败时 rssi 列留空
    #SUMMARY case_id=6 label=FOILGAP n=50 median=-58.00 mean=-58.30 stdev=2.10 min=-63 max=-55 valid=50
    #END
    #OK C
    [DISP] [B]C6 FOILGAP rssi=-58 med=-58.0 base=-45.0 smp=50/50
"""

from __future__ import annotations

import csv
import re
import time
from collections import Counter
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

DATA_COLUMNS: Tuple[str, ...] = ("case_id", "seq", "t_ms", "rssi_dbm")
DISPLAY_PREFIX = "[DISP]"

#: 命令行允许的采样数范围，与 board b/main/app_config.h 保持一致
N_MIN, N_MAX = 10, 500
#: 采样间隔范围（ms），与 app_config.h 保持一致
INTERVAL_MIN_MS, INTERVAL_MAX_MS = 20, 1000


# ---------------------------------------------------------------------------
# 异常
# ---------------------------------------------------------------------------


class SerialProtocolError(RuntimeError):
    """串口 / 协议层错误：打不开端口、命令被拒、数据块不完整等。"""


class LinkTimeout(SerialProtocolError):
    """在限定时间内没有读到期望的行。"""


# ---------------------------------------------------------------------------
# 行分类
# ---------------------------------------------------------------------------


def split_csv(line: str) -> Optional[List[str]]:
    """按 csv 规则切分一行；非法引号等 CSV 错误返回 ``None``。"""
    try:
        rows = list(csv.reader([line], strict=True))
    except csv.Error:
        return None
    if len(rows) != 1:
        return None
    return rows[0]


def is_data_header(line: str) -> bool:
    fields = split_csv(line)
    if fields is None or len(fields) != len(DATA_COLUMNS):
        return False
    return tuple(f.strip() for f in fields) == DATA_COLUMNS


def parse_data_row(line: str) -> Optional[dict]:
    """把 ``8,1,0,-58`` 解析成 dict；不是合法 4 列数据行时返回 ``None``。

    rssi 列为空（采集失败）时 ``rssi_dbm`` 为 ``None``，该行仍然保留，
    由分析脚本统计为无效样本。
    """
    fields = split_csv(line)
    if fields is None or len(fields) != len(DATA_COLUMNS):
        return None
    try:
        case_id = int(fields[0])
        seq = int(fields[1])
        t_ms = int(fields[2])
    except ValueError:
        return None

    raw_rssi = fields[3].strip()
    if raw_rssi == "":
        rssi: Optional[int] = None
    else:
        try:
            rssi = int(raw_rssi)
        except ValueError:
            return None

    return {"case_id": case_id, "seq": seq, "t_ms": t_ms, "rssi_dbm": rssi}


def classify_line(line: str) -> Tuple[str, object]:
    """把一行分成 7 类，供采集脚本按上下文处理。

    返回值 ``(kind, payload)``：

    ==========  =====================================================
    kind        payload
    ==========  =====================================================
    disp        None（``[DISP]`` 屏幕同步日志，必须丢弃）
    blank       None
    meta        ``#`` 开头的协议行原文
    header      主数据表表头（``case_id,seq,t_ms,rssi_dbm``）
    data        4 列数据行的 dict（见 :func:`parse_data_row`）
    pair        2 列行（``#CASELIST`` / ``#STALIST`` 块内的数据）
    malformed   无法归类的行（必须计数上报）
    ==========  =====================================================
    """
    text = line.strip()
    if not text:
        return "blank", None
    if text.startswith(DISPLAY_PREFIX):
        return "disp", None
    if text.startswith("#"):
        return "meta", text
    if is_data_header(text):
        return "header", None
    row = parse_data_row(text)
    if row is not None:
        return "data", row
    fields = split_csv(text)
    if fields is not None and len(fields) == 2:
        return "pair", fields
    return "malformed", text


# ---------------------------------------------------------------------------
# 元数据解析
# ---------------------------------------------------------------------------

_KV_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]*)")


def parse_kv(line: str) -> Dict[str, str]:
    """把 ``#BEGIN case_id=0 label=NONE n=50 ...`` 解析成字典（含首个 token）。"""
    parts = line.strip().split()
    out: Dict[str, str] = {}
    if parts:
        out["tag"] = parts[0]
    for key, value in _KV_RE.findall(line):
        out[key] = value
    return out


@dataclass
class ApInfo:
    """``#APINFO`` 的内容。板 A 是 txp，板 B 是 rssi，两者字段不完全相同。"""

    ssid: str = ""
    bssid: str = ""
    channel: Optional[int] = None
    rssi: Optional[int] = None
    txp: Optional[float] = None
    raw: str = ""


def parse_apinfo(line: str) -> ApInfo:
    kv = parse_kv(line)
    info = ApInfo(raw=line.strip())
    info.ssid = kv.get("ssid", "")
    info.bssid = normalize_mac(kv.get("bssid", ""))

    ch = kv.get("ch", "")
    if ch.lstrip("-").isdigit():
        info.channel = int(ch)

    rssi = kv.get("rssi", "")
    if rssi.lstrip("-").isdigit():
        info.rssi = int(rssi)

    txp = kv.get("txp", "")
    if txp:
        try:
            info.txp = float(txp)
        except ValueError:
            info.txp = None
    return info


def parse_begin(line: str) -> dict:
    """解析 ``#BEGIN case_id=0 label=NONE n=50 interval_ms=100 t0_ms=...``。"""
    kv = parse_kv(line)
    try:
        return {
            "case_id": int(kv["case_id"]),
            "label": kv.get("label", ""),
            "n": int(kv["n"]),
            "interval_ms": int(kv.get("interval_ms", "0")),
            "t0_ms": int(kv.get("t0_ms", "0")),
        }
    except (KeyError, ValueError) as exc:
        raise SerialProtocolError(f"#BEGIN 行字段不全：{line!r}") from exc


def parse_summary(line: str) -> dict:
    """解析 ``#SUMMARY case_id=6 label=FOILGAP n=50 median=-58.00 ...``。"""
    kv = parse_kv(line)
    try:
        return {
            "case_id": int(kv["case_id"]),
            "label": kv.get("label", ""),
            "n": int(kv["n"]),
            "median": float(kv["median"]),
            "mean": float(kv["mean"]),
            "stdev": float(kv["stdev"]),
            "min": int(kv["min"]),
            "max": int(kv["max"]),
            "valid": int(kv["valid"]),
        }
    except (KeyError, ValueError) as exc:
        raise SerialProtocolError(f"#SUMMARY 行字段不全：{line!r}") from exc


def parse_txp(line: str) -> dict:
    """解析 ``#TXP set=8.5 raw=34`` / ``#TXP get=20.0 raw=80``。"""
    kv = parse_kv(line)
    out: Dict[str, object] = {"raw": None}
    for key in ("set", "get"):
        if key in kv:
            try:
                out[key] = float(kv[key])
            except ValueError:
                out[key] = None
    if kv.get("raw", "").lstrip("-").isdigit():
        out["raw"] = int(kv["raw"])
    return out


def normalize_mac(mac: str) -> str:
    """统一成 ``aa:bb:cc:dd:ee:ff``（小写、冒号分隔）。"""
    return mac.strip().lower().replace("-", ":")


# ---------------------------------------------------------------------------
# 解析统计（不许静默跳过）
# ---------------------------------------------------------------------------


@dataclass
class ParseStats:
    """逐行统计，采集结束必须打印出来。"""

    lines_total: int = 0
    disp_dropped: int = 0
    meta: int = 0
    header: int = 0
    data_rows: int = 0
    pairs: int = 0
    blank: int = 0
    malformed: int = 0
    malformed_samples: List[str] = field(default_factory=list)
    meta_tags: Counter = field(default_factory=Counter)

    def note(self, kind: str, line: str = "") -> None:
        self.lines_total += 1
        if kind == "disp":
            self.disp_dropped += 1
        elif kind == "blank":
            self.blank += 1
        elif kind == "meta":
            self.meta += 1
            tag = line.strip().split(" ", 1)[0] if line.strip() else "#"
            self.meta_tags[tag] += 1
        elif kind == "header":
            self.header += 1
        elif kind == "data":
            self.data_rows += 1
        elif kind == "pair":
            self.pairs += 1
        else:
            self.note_malformed(line)

    def note_malformed(self, line: str) -> None:
        self.malformed += 1
        if len(self.malformed_samples) < 5:
            self.malformed_samples.append(line.strip())

    def report(self) -> str:
        tags = " ".join(f"{tag}×{count}" for tag, count in sorted(self.meta_tags.items()))
        lines = [
            "串口解析统计：",
            f"  总行数 {self.lines_total} | [DISP] 丢弃 {self.disp_dropped} | "
            f"元数据 {self.meta} | 表头 {self.header} | CSV 数据 {self.data_rows} | "
            f"键值对 {self.pairs} | 空行 {self.blank} | 解析失败 {self.malformed}",
        ]
        if tags:
            lines.append(f"  元数据明细：{tags}")
        if self.malformed_samples:
            lines.append("  解析失败示例（最多 5 条）：")
            lines.extend(f"    {sample!r}" for sample in self.malformed_samples)
        return "\n".join(lines)


# ---------------------------------------------------------------------------
# 串口链路
# ---------------------------------------------------------------------------


class SerialLink:
    """带行缓冲的串口封装，接口与测试用 FakeBoard 一致。

    只提供 ``send`` / ``read_line`` / ``flush_input`` / ``close`` 四个方法，
    采集脚本只依赖这四个方法，离线自测时可以换成假串口。
    """

    def __init__(self, port: str, baud: int = 115200, label: str = "",
                 read_timeout: float = 0.05) -> None:
        try:
            import serial  # 延迟导入：解析与自测不需要 pyserial
        except ModuleNotFoundError as exc:  # pragma: no cover - 依赖环境
            raise SerialProtocolError(
                "缺少 pyserial：请先执行 python -m pip install -r requirements.txt"
            ) from exc

        self.port = port
        self.label = label or port
        self._read_timeout = read_timeout
        self._buf = bytearray()
        self._closed = False
        try:
            self._ser = serial.Serial(port=port, baudrate=baud,
                                      bytesize=8, parity="N", stopbits=1,
                                      timeout=read_timeout)
        except (serial.SerialException, OSError) as exc:
            raise SerialProtocolError(
                f"打不开串口 {port}：{exc}\n"
                "  · 先关掉 idf.py monitor / 串口助手 / 其它占用该端口的程序；\n"
                "  · 确认设备管理器里确实是这个 COM 号（拔掉一块板看哪个消失）；\n"
                "  · 两块板各占一路 USB，别把 A/B 接反。"
            ) from exc
        # 丢掉上电日志 / 上一轮残留，保证命令-应答对齐
        try:
            self._ser.reset_input_buffer()
        except (serial.SerialException, OSError):
            pass

    # -- 链路操作 ---------------------------------------------------------
    def send(self, command: str) -> None:
        if self._closed:
            raise SerialProtocolError(f"串口 {self.port} 已关闭，无法发送 {command!r}")
        try:
            payload = (command + "\n").encode("ascii")
        except UnicodeEncodeError as exc:
            raise SerialProtocolError(f"命令只能包含 ASCII 字符：{command!r}") from exc
        try:
            self._ser.write(payload)
            self._ser.flush()
        except (serial.SerialException, OSError) as exc:  # pragma: no cover
            raise SerialProtocolError(f"串口 {self.port} 写入失败：{exc}") from exc

    def flush_input(self) -> None:
        self._buf.clear()
        if not self._closed:
            try:
                self._ser.reset_input_buffer()
            except (serial.SerialException, OSError):  # pragma: no cover
                pass

    def read_line(self, timeout: float) -> str:
        """读一行（不含 ``\n``）；超时抛 :class:`LinkTimeout`，半行留在缓冲里。"""
        if self._closed:
            raise SerialProtocolError(f"串口 {self.port} 已关闭")

        deadline = time.monotonic() + max(0.0, timeout)
        while True:
            idx = self._buf.find(b"\n")
            if idx >= 0:
                raw = bytes(self._buf[:idx])
                del self._buf[:idx + 1]
                return raw.decode("utf-8", errors="replace").rstrip("\r")

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                partial = bytes(self._buf).decode("utf-8", errors="replace")
                extra = f"（缓冲中的半行：{partial!r}）" if partial else ""
                raise LinkTimeout(f"串口 {self.port} 读行超时{extra}")

            try:
                chunk = self._ser.read(1)
                if chunk:
                    # 一次把已经到达的字节全部取走，减少系统调用
                    pending = self._ser.in_waiting
                    if pending:
                        chunk += self._ser.read(pending)
            except (serial.SerialException, OSError) as exc:  # pragma: no cover
                raise SerialProtocolError(f"串口 {self.port} 读取失败：{exc}") from exc
            if chunk:
                self._buf.extend(chunk)

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._ser.close()
        except (serial.SerialException, OSError):  # pragma: no cover
            pass


# ---------------------------------------------------------------------------
# 命令-应答
# ---------------------------------------------------------------------------


def _read_before(link, deadline: float, what: str) -> str:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise LinkTimeout(
            f"等待 {what} 超时：板子在限定时间内没有给出应答。\n"
            "  · 确认用的是对的固件（板 B 才有 C=/CL/N=/P）；\n"
            "  · 确认波特率 115200、串口没被 monitor 占用；\n"
            "  · 按一下板子复位键再试。"
        )
    return link.read_line(remaining)


def query_caselist(link, stats: ParseStats, timeout: float = 8.0) -> Dict[int, str]:
    """发 ``CL``，把 ``#CASELIST`` 块解析成 ``{case_id: label}``。"""
    link.flush_input()
    link.send("CL")
    deadline = time.monotonic() + timeout
    declared: Optional[int] = None
    cases: Dict[int, str] = {}

    while True:
        line = _read_before(link, deadline, "#CASELIST / #OK CL")
        kind, payload = classify_line(line)
        stats.note(kind, line)

        if line.startswith("#CASELIST"):
            kv = parse_kv(line)
            try:
                declared = int(kv["n"])
            except (KeyError, ValueError) as exc:
                raise SerialProtocolError(f"#CASELIST 行缺少 n=：{line!r}") from exc
        elif line.startswith("#OK CL"):
            break
        elif line.startswith("#ERR"):
            raise SerialProtocolError(f"板子拒绝了 CL 命令：{line}")
        elif kind == "pair":
            assert isinstance(payload, list)
            case_id_text, label = payload[0].strip(), payload[1].strip()
            if not case_id_text.isdigit():
                stats.note_malformed(line)
                continue
            cases[int(case_id_text)] = label

    if declared is None:
        raise SerialProtocolError("没有收到 #CASELIST 行，无法建立 case_id → label 映射")
    if declared != len(cases):
        raise SerialProtocolError(
            f"#CASELIST n={declared}，但实际只解析到 {len(cases)} 条测点：{cases}"
        )
    expected = set(range(declared))
    if set(cases) != expected:
        raise SerialProtocolError(
            f"测点编号不连续：收到 {sorted(cases)}，期望 {sorted(expected)}"
        )
    return cases


def _wait_ok(link, stats: ParseStats, ok_token: str, what: str,
             deadline: float) -> List[str]:
    """读到 ``ok_token`` 为止，返回期间收到的所有行（遇 #ERR 直接报错）。"""
    collected: List[str] = []
    while True:
        line = _read_before(link, deadline, f"{what} 的 {ok_token}")
        kind, _payload = classify_line(line)
        stats.note(kind, line)
        collected.append(line)

        if line.startswith(ok_token):
            return collected
        if line.startswith("#ERR"):
            raise SerialProtocolError(f"{what} 被固件拒绝：{line}")


def set_sample_count(link, stats: ParseStats, n: int, timeout: float = 5.0) -> None:
    if not (N_MIN <= n <= N_MAX):
        raise SerialProtocolError(f"--n 必须在 {N_MIN}..{N_MAX} 之间，收到 {n}")
    link.flush_input()
    link.send(f"N={n}")
    _wait_ok(link, stats, "#OK N", f"N={n}", time.monotonic() + timeout)


def set_sample_interval(link, stats: ParseStats, interval_ms: int,
                        timeout: float = 5.0) -> None:
    if not (INTERVAL_MIN_MS <= interval_ms <= INTERVAL_MAX_MS):
        raise SerialProtocolError(
            f"--interval 必须在 {INTERVAL_MIN_MS}..{INTERVAL_MAX_MS} ms 之间，"
            f"收到 {interval_ms}"
        )
    link.flush_input()
    link.send(f"I={interval_ms}")
    _wait_ok(link, stats, "#OK I", f"I={interval_ms}", time.monotonic() + timeout)


def query_apinfo(link, stats: ParseStats, timeout: float = 8.0) -> ApInfo:
    """发 ``P``，返回板子当前的 ``#APINFO``。"""
    link.flush_input()
    link.send("P")
    deadline = time.monotonic() + timeout
    info: Optional[ApInfo] = None

    while True:
        line = _read_before(link, deadline, "#APINFO / #OK P")
        kind, _payload = classify_line(line)
        stats.note(kind, line)
        if line.startswith("#APINFO"):
            info = parse_apinfo(line)
        elif line.startswith("#OK P"):
            break
        elif line.startswith("#ERR"):
            raise SerialProtocolError(f"板子拒绝了 P 命令：{line}")

    if info is None:
        raise SerialProtocolError(
            "板子没有回 #APINFO：板 B 可能还没连上板 A 的热点。"
            "先确认板 B 的 P 命令输出里有 bssid 再采集。"
        )
    return info


def set_tx_power(link, stats: ParseStats, dbm: float,
                 timeout: float = 8.0) -> Optional[float]:
    """发 ``T=<dBm>``（仅板 A），返回固件实际生效的功率。"""
    link.flush_input()
    link.send(f"T={dbm:g}")
    deadline = time.monotonic() + timeout
    applied: Optional[float] = None

    while True:
        line = _read_before(link, deadline, "#TXP set= / #OK T")
        kind, _payload = classify_line(line)
        stats.note(kind, line)
        if line.startswith("#TXP"):
            kv = parse_txp(line)
            if "set" in kv:
                applied = kv.get("set")  # type: ignore[assignment]
        elif line.startswith("#OK T"):
            break
        elif line.startswith("#ERR"):
            raise SerialProtocolError(
                f"设置发射功率 T={dbm:g} 失败：{line}\n"
                "  固件允许范围 2.0..20.0 dBm，步进 0.25 dBm。"
            )
    return applied


def query_tx_power(link, stats: ParseStats, timeout: float = 5.0) -> Optional[float]:
    """发 ``G``（仅板 A），返回当前最大发射功率 dBm。"""
    link.flush_input()
    link.send("G")
    deadline = time.monotonic() + timeout
    dbm: Optional[float] = None

    while True:
        line = _read_before(link, deadline, "#TXP get= / #OK G")
        kind, _payload = classify_line(line)
        stats.note(kind, line)
        if line.startswith("#TXP"):
            kv = parse_txp(line)
            if "get" in kv:
                dbm = kv.get("get")  # type: ignore[assignment]
        elif line.startswith("#OK G"):
            break
        elif line.startswith("#ERR"):
            raise SerialProtocolError(f"读取发射功率失败：{line}")
    return dbm


@dataclass
class StaList:
    """一次 ``L`` 命令的结果。"""

    macs: Dict[str, Optional[int]]
    seq: Optional[int] = None
    t_host: float = 0.0               # 上位机收到第一行 STA 数据时的 monotonic 时间
    t_device_ms: Optional[int] = None  # 板 A 的 #STALIST_BEGIN t_ms


def read_stalist(link, stats: ParseStats, timeout: float = 8.0) -> StaList:
    """发 ``L``（仅板 A），解析 ``#STALIST`` 块。

    块格式::

        #STALIST_BEGIN i=12 t_ms=456789
        #STALIST n=1
        mac,rssi
        aa:bb:cc:dd:ee:ff,-45
        #OK L
    """
    link.flush_input()
    link.send("L")
    deadline = time.monotonic() + timeout
    started = False
    seq: Optional[int] = None
    t_device_ms: Optional[int] = None
    t_host = 0.0
    macs: Dict[str, Optional[int]] = {}

    while True:
        line = _read_before(link, deadline, "#STALIST_BEGIN / #OK L")
        kind, payload = classify_line(line)
        stats.note(kind, line)

        if line.startswith("#STALIST_BEGIN"):
            kv = parse_kv(line)
            started = True
            if kv.get("i", "").isdigit():
                seq = int(kv["i"])
            if kv.get("t_ms", "").lstrip("-").isdigit():
                t_device_ms = int(kv["t_ms"])
        elif line.startswith("#OK L"):
            break
        elif line.startswith("#ERR"):
            raise SerialProtocolError(f"板子拒绝了 L 命令：{line}")
        elif kind == "pair" and started:
            assert isinstance(payload, list)
            mac_text, rssi_text = payload[0].strip(), payload[1].strip()
            if mac_text.lower() == "mac":      # 表头 mac,rssi
                continue
            if not mac_text or mac_text.isdigit():
                stats.note_malformed(line)
                continue
            try:
                rssi = int(rssi_text)
            except ValueError:
                stats.note_malformed(line)
                continue
            macs[normalize_mac(mac_text)] = rssi
            if t_host == 0.0:
                t_host = time.monotonic()

    if not started:
        raise SerialProtocolError("没有收到 #STALIST_BEGIN，无法确认板 A 的 STA 列表块")
    return StaList(macs=macs, seq=seq, t_host=t_host, t_device_ms=t_device_ms)


def pick_sta(stalist: StaList, preferred_mac: Optional[str] = None,
             warn=print) -> Tuple[str, Optional[int]]:
    """从 ``#STALIST`` 里挑出目标 STA。

    · 给了 ``--sta-mac``：必须精确命中，否则报错；
    · 只连了 1 个 STA：直接用它；
    · 连了多个：取 RSSI 最大的并显式警告（手机等设备可能也连着这个热点）。
    """
    if not stalist.macs:
        raise SerialProtocolError(
            "板 A 的 #STALIST 为空：板 B 还没连上软 AP，或者 UDP 保活没开（U=1）。"
        )

    if preferred_mac:
        wanted = normalize_mac(preferred_mac)
        if wanted not in stalist.macs:
            raise SerialProtocolError(
                f"--sta-mac {wanted} 不在板 A 的 STA 列表里；"
                f"当前列表：{sorted(stalist.macs)}"
            )
        return wanted, stalist.macs[wanted]

    if len(stalist.macs) == 1:
        mac = next(iter(stalist.macs))
        return mac, stalist.macs[mac]

    ordered = sorted(stalist.macs.items(),
                     key=lambda item: item[1] if item[1] is not None else -999,
                     reverse=True)
    warn(
        "[WARN] 板 A 同时看到多个 STA，无法确定哪一个是板 B："
        f"{ordered}；本次取 RSSI 最大的 {ordered[0][0]}。"
        "建议下次用 --sta-mac 明确指定。"
    )
    return ordered[0]
