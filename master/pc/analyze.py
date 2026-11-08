#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""遮挡穿透损耗分析（对应 ``提示词-C-上位机.md`` 需求二）。

输入（默认都在 ``pc/data/``）：

* ``raw.csv``        —— ``capture.py`` 采到的原始样本（含首尾两次基准）
* ``materials.csv``  —— 手填的材质表（case_id → material / thickness / category）

输出：

* ``data/summary.csv``        —— 每个测点的中位数、损耗与不确定度
* ``data/compare_theory.csv`` —— 实测损耗 vs 经验量级 + 判定
* ``figures/loss_by_material.png``
* ``figures/baseline_drift.png``
* ``figures/loss_by_thickness.png``（仅 dielectric 类有 ≥2 个不同厚度时）

核心规则（提示词 0.2）：

1. 基准取首尾两次 case 0 中位数的平均；
2. 首尾基准差 > 3 dB → **拒绝出图并报错**，不静默照算；
3. ``σ_loss = sqrt(σ_base² + σ_case²)``，误差棒用它。
"""

from __future__ import annotations

import argparse
import importlib
import math
import sys
from datetime import date
from pathlib import Path
from typing import Dict, List, Optional, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent

#: 首尾基准中位数差超过此值 → 整批数据作废
DRIFT_LIMIT_DB = 3.0
#: 离群剔除阈值：|rssi − 组中位数| > 3 × MAD
MAD_K = 3.0
#: 与参考量级比较时的容差
VERDICT_TOL_DB = 3.0

RAW_REQUIRED = ["case_id", "label", "seq", "t_ms", "rssi_dbm",
                "round_idx", "round_role", "ap_bssid", "ap_ch"]
MATERIAL_REQUIRED = ["case_id", "label", "material", "thickness_mm",
                     "category", "note"]
CATEGORIES = ("reference", "dielectric", "water", "conductor", "mesh")

#: 经验量级（dB），非严格理论计算 —— 提示词明确要求写死在脚本里
#: 纸板/玻璃按 dielectric；书堆/水瓶/人体按 water；铝箔按 conductor。
REFERENCE_DB: Dict[str, Tuple[float, float]] = {
    "dielectric": (0.5, 5.0),
    "water": (3.0, 15.0),
    "conductor": (20.0, 40.0),
    "mesh": (15.0, 30.0),
}

BASE_ROLES = ("first_base", "last_base")


class AnalysisError(RuntimeError):
    """分析流程错误：缺文件、缺列、基准漂移等，必须让人看到原因。"""


# ---------------------------------------------------------------------------
# 依赖与输入
# ---------------------------------------------------------------------------


def import_stack():
    """延迟导入第三方库，保证 ``--help`` 在没有 pandas 时也能看。"""
    missing: List[str] = []
    modules = {}
    for name in ("pandas", "numpy", "matplotlib"):
        try:
            modules[name] = importlib.import_module(name)
        except ModuleNotFoundError:
            missing.append(name)
    if missing:
        raise AnalysisError(
            "缺少依赖：" + ", ".join(missing) + "\n"
            "  在 pc/ 目录执行：python -m pip install -r requirements.txt\n"
            "  （analyze.py 必须依赖 pandas + matplotlib；只跑 capture 的离线自测可以不装）"
        )
    return modules["pandas"], modules["numpy"], modules["matplotlib"]


def load_raw(pd, path: Path):
    """读 raw.csv，返回 ``(DataFrame, 空 rssi 行数)``。"""
    if not path.exists():
        raise AnalysisError(
            f"找不到原始数据 {path}\n"
            "  先在 pc/ 目录运行：python capture.py --port COM5 --cases 0-10 --n 50\n"
            "  （路径可用 --input 指定）"
        )
    try:
        frame = pd.read_csv(path, dtype={"label": str, "round_role": str, "ap_bssid": str})
    except pd.errors.EmptyDataError as exc:
        raise AnalysisError(f"{path} 是空文件，没有采集到任何数据。") from exc

    missing = [column for column in RAW_REQUIRED if column not in frame.columns]
    if missing:
        raise AnalysisError(
            f"{path} 缺列 {missing}；实际列 = {list(frame.columns)}。\n"
            "  这是旧格式或手工改过的文件，请用当前 capture.py 重新采集。"
        )

    frame["rssi_dbm"] = pd.to_numeric(frame["rssi_dbm"], errors="coerce")
    frame["case_id"] = pd.to_numeric(frame["case_id"], errors="raise").astype(int)
    frame["round_role"] = frame["round_role"].astype(str).str.strip()

    invalid = int(frame["rssi_dbm"].isna().sum())
    if len(frame) == 0 or invalid == len(frame):
        raise AnalysisError(
            f"{path} 里没有任何有效 RSSI（{invalid}/{len(frame)} 行为空）。\n"
            "  原因通常是板 B 没连上板 A 的热点，或采集时板子掉线。"
        )

    unexpected = sorted(set(frame["round_role"]) - {"case", *BASE_ROLES})
    if unexpected:
        raise AnalysisError(
            f"{path} 的 round_role 出现未知取值 {unexpected}（应为 first_base/case/last_base）。"
        )
    return frame, invalid


def load_materials(pd, path: Path, raw_cases: Dict[int, str], warn=print):
    """读手工材质表并做一致性校验（case_id 与固件测点表严格对应）。"""
    if not path.exists():
        raise AnalysisError(
            f"找不到材质表 {path}\n"
            "  模板在 data/materials.csv，按实际遮挡物填好后用 --materials 指定。"
        )
    try:
        table = pd.read_csv(path, dtype={"label": str, "material": str,
                                         "category": str, "note": str})
    except pd.errors.EmptyDataError as exc:
        raise AnalysisError(f"{path} 是空文件，至少要有表头和基准行。") from exc

    missing = [column for column in MATERIAL_REQUIRED if column not in table.columns]
    if missing:
        raise AnalysisError(f"{path} 缺列 {missing}；模板列 = {MATERIAL_REQUIRED}")

    table = table.dropna(subset=["case_id"]).copy()
    table["case_id"] = pd.to_numeric(table["case_id"], errors="raise").astype(int)
    table["thickness_mm"] = pd.to_numeric(table["thickness_mm"], errors="raise")

    duplicated = sorted(set(table["case_id"][table["case_id"].duplicated()]))
    if duplicated:
        raise AnalysisError(f"{path} 里 case_id 重复：{duplicated}，一行只能对应一个测点。")

    unknown = sorted(set(table["category"].astype(str).str.strip()) - set(CATEGORIES))
    if unknown:
        raise AnalysisError(
            f"{path} 的 category 有未知取值 {unknown}；允许：{list(CATEGORIES)}"
        )
    table["category"] = table["category"].astype(str).str.strip()

    missing_cases = sorted(set(raw_cases) - set(table["case_id"]))
    if missing_cases:
        details = ", ".join(f"C{case}={raw_cases[case]}" for case in missing_cases)
        raise AnalysisError(
            f"{path} 缺少本次采集的测点：{details}\n"
            "  每个采到的 case_id 都必须在材质表里有一行，否则无法算损耗。"
        )
    extra = sorted(set(table["case_id"]) - set(raw_cases))
    if extra:
        warn(f"[WARN] 材质表里的 {extra} 本次没有采集，已忽略（不影响出图）。")

    for _, row in table.iterrows():
        case = int(row["case_id"])
        if case in raw_cases and str(row["label"]).strip() != raw_cases[case]:
            raise AnalysisError(
                f"case {case} 的 label 不一致：raw.csv={raw_cases[case]!r}，"
                f"materials.csv={str(row['label']).strip()!r}。\n"
                "  固件测点表改过却没同步材质表，这个 case 的结论不可信。"
            )
    return table.reset_index(drop=True)


# ---------------------------------------------------------------------------
# 清洗与统计
# ---------------------------------------------------------------------------


def remove_outliers(pd, frame):
    """按测点（round_idx）分组剔除 |rssi − 中位数| > 3×MAD 的样本。

    返回 ``(cleaned, report)``；report 逐轮记录剔除数，绝不静默。
    """
    cleaned_parts = []
    report: List[dict] = []

    for round_idx, group in frame.groupby("round_idx", sort=True):
        values = group["rssi_dbm"]
        valid = values.dropna()
        median = float(valid.median())
        mad = float((valid - median).abs().median())
        if mad > 0:
            # NaN（采集失败）不是离群点，保留并按无效样本单独统计
            keep_mask = values.isna() | ((values - median).abs() <= MAD_K * mad)
            keep = group[keep_mask]
        else:
            # 全部相同（MAD=0）时不能按 0 阈值剔除任何点，否则会误删正常样本
            keep = group
        dropped = len(group) - len(keep)
        cleaned_parts.append(keep)
        report.append({
            "round_idx": int(round_idx),
            "case_id": int(group["case_id"].iloc[0]),
            "role": str(group["round_role"].iloc[0]),
            "n_in": int(len(group)),
            "n_valid": int(len(valid)),
            "n_out": int(dropped),
            "median": median,
            "mad": mad,
        })

    cleaned = (pd.concat(cleaned_parts).sort_index()
               if cleaned_parts else frame.iloc[0:0])
    return cleaned, report


def base_statistics(pd, frame):
    """首尾两次基准：返回基准统计与漂移；漂移超限直接抛错。"""
    bases = frame[frame["round_role"].isin(BASE_ROLES)]
    present = sorted(set(bases["round_role"]))
    missing = [role for role in BASE_ROLES if role not in present]
    if missing:
        counts = frame["round_role"].value_counts().to_dict()
        raise AnalysisError(
            f"缺基准轮次 {missing}（当前计数 {counts}）。\n"
            "  每批数据必须首尾各测一次基准：capture.py 遇到掉线时要整批重测。"
        )
    if bases["round_role"].nunique() != 2:
        raise AnalysisError("基准分组异常：不止 first_base / last_base 两种 round_role。")

    stats: Dict[str, dict] = {}
    for role in BASE_ROLES:
        values = bases.loc[bases["round_role"] == role, "rssi_dbm"].dropna()
        if values.empty:
            raise AnalysisError(f"{role} 没有任何有效样本。")
        stats[role] = {
            "median": float(values.median()),
            "stdev": float(values.std(ddof=1)) if len(values) > 1 else 0.0,
            "n": int(len(values)),
            "values": values,
        }

    drift = stats["last_base"]["median"] - stats["first_base"]["median"]
    return stats, drift


def ensure_drift_ok(stats: Dict[str, dict], drift: float) -> None:
    """首尾基准差 > 3 dB → 拒绝出图（提示词 0.2 第 2 条）。"""
    if abs(drift) <= DRIFT_LIMIT_DB:
        return
    first = stats["first_base"]["median"]
    last = stats["last_base"]["median"]
    raise AnalysisError(
        f"基准漂移 {drift:+.2f} dB（首 {first:.2f} → 尾 {last:.2f} dBm），"
        f"超过 ±{DRIFT_LIMIT_DB:g} dB 判据，整批数据作废，拒绝出图。\n"
        "  可能原因：\n"
        "    · 采集中有人/物挡在链路中间（人体对 2.4 GHz 衰减 10 dB 以上）；\n"
        "    · 板子/遮挡物位置被碰动，或换了 AP / 信道；\n"
        "    · 环境里新开了 Wi-Fi、微波炉等干扰源；\n"
        "    · 板 B 发射功率或自动速率变化。\n"
        "  处理：把遮挡物恢复到初始位置，重新跑一遍 capture.py（首尾基准会自动重采）。"
    )


def compute_summary(pd, frame, materials,
                    stats: Dict[str, dict]) -> Tuple[List[dict], float, float]:
    """按 ``loss = median(base) − median(case)`` 计算每个遮挡测点。

    基准中位数 = 首尾两次中位数的平均；σ_base 取两次标准差的均方根。
    """
    base_median = (stats["first_base"]["median"] + stats["last_base"]["median"]) / 2.0
    sigma_base = math.sqrt(
        (stats["first_base"]["stdev"] ** 2 + stats["last_base"]["stdev"] ** 2) / 2.0
    )

    lookup = materials.set_index("case_id")
    cases = frame[(frame["round_role"] == "case") & frame["rssi_dbm"].notna()]

    rows: List[dict] = []
    for case_id, group in cases.groupby("case_id", sort=True):
        case_id = int(case_id)
        if case_id not in lookup.index:
            raise AnalysisError(f"raw.csv 里的 case {case_id} 不在材质表中。")
        info = lookup.loc[case_id]
        if str(info["category"]) == "reference":
            continue                      # 基准行不进损耗汇总
        values = group["rssi_dbm"]
        median = float(values.median())
        stdev = float(values.std(ddof=1)) if len(values) > 1 else 0.0
        loss = base_median - median
        sigma = math.sqrt(sigma_base ** 2 + stdev ** 2)
        rows.append({
            "case_id": case_id,
            "label": str(info["label"]),
            "material": str(info["material"]),
            "thickness_mm": float(info["thickness_mm"]),
            "category": str(info["category"]),
            "n": int(len(values)),
            "median": median,
            "stdev": stdev,
            "loss_db": loss,
            "sigma_loss": sigma,
        })

    if not rows:
        raise AnalysisError(
            "没有任何非基准测点可算损耗：materials.csv 里除 reference 外至少要有一行，"
            "且 raw.csv 里要有对应的 case 数据。"
        )
    rows.sort(key=lambda row: row["case_id"])
    return rows, base_median, sigma_base


def compare_theory(summary: List[dict]) -> List[dict]:
    """实测 vs 经验量级；容差 ±3 dB，三档判定。"""
    result = []
    for row in summary:
        low, high = REFERENCE_DB[row["category"]]
        measured = row["loss_db"]
        if measured > high + VERDICT_TOL_DB:
            verdict = "超出"
        elif measured < low - VERDICT_TOL_DB:
            verdict = "低于"
        else:
            verdict = "符合"
        result.append({
            "case_id": row["case_id"],
            "label": row["label"],
            "category": row["category"],
            "loss_measured": measured,
            "loss_ref_low": low,
            "loss_ref_high": high,
            "verdict": verdict,
        })
    return result


def write_csv_rows(path: Path, fieldnames: List[str], rows: List[dict],
                   decimals: Tuple[str, ...] = (),
                   compact: Tuple[str, ...] = ()) -> None:
    """写 CSV：保留 2 位小数、整数厚度不带 ``.0``；用 utf-8-sig 便于 Excel 直接打开。"""
    import csv

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            out = {}
            for key in fieldnames:
                value = row[key]
                if key in decimals and isinstance(value, float):
                    out[key] = f"{value:.2f}"
                elif key in compact and isinstance(value, float):
                    out[key] = f"{value:g}"
                else:
                    out[key] = value
            writer.writerow(out)


# ---------------------------------------------------------------------------
# 绘图
# ---------------------------------------------------------------------------

CATEGORY_COLORS = {
    "reference": "#9E9E9E",
    "dielectric": "#4C72B0",
    "water": "#55A868",
    "conductor": "#C44E52",
    "mesh": "#8172B2",
}

CJK_FONT_CANDIDATES = (
    "Microsoft YaHei", "SimHei", "Noto Sans CJK SC",
    "Source Han Sans SC", "WenQuanYi Zen Hei", "PingFang SC",
)

LABELS = {
    "zh": {
        "loss": "穿透损耗 (dB)",
        "rssi": "接收电平 RSSI (dBm)",
        "category": "材质分类",
        "material_title": "各遮挡物穿透损耗（件间距 1.0 m，每测点 {n} 个样本）\n{date}",
        "thickness": "遮挡物厚度 (mm)",
        "thickness_title": "{cat}类遮挡物：损耗与厚度的关系（件间距 1.0 m）\n{date}",
        "slope": "拟合斜率",
        "baseline_title": "基准稳定性：首尾两次无遮挡测量的 RSSI 分布\n{date}",
        "first_base": "首次基准 (first_base)",
        "last_base": "末次基准 (last_base)",
        "delta": "首尾中位数差",
    },
    "en": {
        "loss": "Obstruction loss (dB)",
        "rssi": "RSSI (dBm)",
        "category": "Category",
        "material_title": "Obstruction loss per material (1.0 m link, n={n} samples)\n{date}",
        "thickness": "Thickness (mm)",
        "thickness_title": "{cat} samples: loss vs thickness (1.0 m link)\n{date}",
        "slope": "Fitted slope",
        "baseline_title": "Baseline stability: first vs last unobstructed run\n{date}",
        "first_base": "first_base",
        "last_base": "last_base",
        "delta": "Median difference",
    },
}

CATEGORY_ZH = {
    "reference": "基准", "dielectric": "介质", "water": "含水/吸收",
    "conductor": "良导体", "mesh": "网格",
}


def setup_style(plt, chinese: bool, warn=print) -> bool:
    """配置中文字体；没有中文字体时返回 False，由调用方改用英文标注。"""
    plt.rcParams["axes.unicode_minus"] = False
    if not chinese:
        plt.rcParams["font.sans-serif"] = ["DejaVu Sans"]
        return False

    from matplotlib import font_manager

    available = {font.name for font in font_manager.fontManager.ttflist}
    for name in CJK_FONT_CANDIDATES:
        if name in available:
            plt.rcParams["font.sans-serif"] = [name, "DejaVu Sans"]
            return True

    warn("[WARN] 未找到中文字体（Microsoft YaHei / SimHei 等都没有），"
         "图上改用英文标注以避免出现方块。装好中文字体后重跑即可恢复中文。")
    plt.rcParams["font.sans-serif"] = ["DejaVu Sans"]
    return False


def plot_loss_by_material(plt, np, summary: List[dict], path: Path,
                          labels: Dict[str, str], font_ok: bool, day: str) -> None:
    """水平柱状图：长度 = loss_db，误差棒 = sigma_loss，颜色 = category。"""
    from matplotlib.patches import Patch

    rows = sorted(summary, key=lambda row: row["loss_db"])
    positions = np.arange(len(rows))
    losses = np.array([row["loss_db"] for row in rows])
    sigmas = np.array([row["sigma_loss"] for row in rows])
    colors = [CATEGORY_COLORS.get(row["category"], "#7F7F7F") for row in rows]
    names = [f"C{row['case_id']} {row['label']}" for row in rows]

    counts = sorted({row["n"] for row in rows})
    n_text = str(counts[0]) if len(counts) == 1 else f"{counts[0]}-{counts[-1]}"

    # 测点数会随工况增加（现在 10 个），高度自适应，避免柱子与标签挤在一起
    height = max(5.0, 0.45 * len(rows) + 1.6)
    fig, ax = plt.subplots(figsize=(8, height))
    ax.barh(positions, losses, xerr=sigmas, color=colors, edgecolor="#222222",
            linewidth=0.6, height=0.62,
            error_kw={"ecolor": "#333333", "capsize": 3, "elinewidth": 1.0})
    ax.set_yticks(positions)
    ax.set_yticklabels(names)
    ax.set_xlabel(labels["loss"])
    ax.set_title(labels["material_title"].format(n=n_text, date=day))
    ax.grid(axis="x", linestyle=":", alpha=0.45)

    span = float(losses.max() + sigmas.max()) if len(rows) else 1.0
    ax.set_xlim(0, max(span * 1.35, 1.0))
    for position, row in zip(positions, rows):
        text = f"{row['loss_db']:.1f} dB"
        if row["thickness_mm"] > 0:
            text += f"\n{row['thickness_mm']:g} mm"
        ax.text(row["loss_db"] + row["sigma_loss"] + span * 0.02, position,
                text, va="center", fontsize=8)

    used = []
    for category in CATEGORIES:
        if any(row["category"] == category for row in rows):
            name = CATEGORY_ZH.get(category, category) if font_ok else category
            used.append(Patch(facecolor=CATEGORY_COLORS[category], label=name))
    ax.legend(handles=used, title=labels["category"], loc="lower right", fontsize=8)

    fig.tight_layout()
    fig.savefig(path, dpi=300, facecolor="white")
    plt.close(fig)


def plot_baseline_drift(plt, np, stats: Dict[str, dict], drift: float,
                        path: Path, labels: Dict[str, str], day: str) -> None:
    """首尾两次基准的分布（箱线 + 散点 + 中位数线），直观展示环境稳定性。"""
    first = stats["first_base"]["values"].to_numpy()
    last = stats["last_base"]["values"].to_numpy()

    fig, ax = plt.subplots(figsize=(6, 4))
    box = ax.boxplot([first, last], widths=0.5, patch_artist=True,
                     showfliers=False, medianprops={"color": "#B22222", "linewidth": 1.4})
    for patch, color in zip(box["boxes"], ("#4C72B0", "#DD8452")):
        patch.set_facecolor(color)
        patch.set_alpha(0.45)

    rng = np.random.default_rng(0)
    for index, values in enumerate((first, last), start=1):
        jitter = rng.uniform(-0.13, 0.13, size=len(values))
        ax.scatter(np.full(len(values), index) + jitter, values,
                   s=12, color="#333333", alpha=0.5, zorder=3)
        ax.hlines(stats["first_base" if index == 1 else "last_base"]["median"],
                  index - 0.28, index + 0.28, color="#B22222", linewidth=1.4, zorder=4)

    ax.set_xticks([1, 2])
    ax.set_xticklabels([labels["first_base"], labels["last_base"]])
    ax.set_ylabel(labels["rssi"])
    ax.set_title(labels["baseline_title"].format(date=day))
    ax.grid(axis="y", linestyle=":", alpha=0.45)

    low = float(min(first.min(), last.min()))
    high = float(max(first.max(), last.max()))
    ax.set_ylim(low - 1.5, high + 0.6)       # 底部留白，避免标注挡住 -48 的散点

    verdict = "OK" if abs(drift) <= DRIFT_LIMIT_DB else "FAIL"
    ax.text(0.02, 0.03,
            f"{labels['delta']} = {drift:+.2f} dB  "
            f"(|Δ| ≤ {DRIFT_LIMIT_DB:g} dB → {verdict})",
            transform=ax.transAxes, fontsize=9,
            bbox={"facecolor": "white", "edgecolor": "#999999", "alpha": 0.85})

    fig.tight_layout()
    fig.savefig(path, dpi=300, facecolor="white")
    plt.close(fig)


def plot_loss_by_thickness(plt, np, summary: List[dict], path: Path,
                           labels: Dict[str, str], day: str,
                           warn=print) -> bool:
    """损耗–厚度散点 + 线性拟合（仅当某类有 ≥2 个不同厚度时）。

    优先 dielectric；模板里的厚度序列（BOOK1/BOOK5）属于 water，所以 water
    有 ≥2 个厚度时也画，并在标题里注明实际用的是哪一类。
    """
    by_category: Dict[str, List[dict]] = {}
    for row in summary:
        if row["thickness_mm"] > 0:
            by_category.setdefault(row["category"], []).append(row)

    qualified = {name: rows for name, rows in by_category.items()
                 if len({row["thickness_mm"] for row in rows}) >= 2}
    if not qualified:
        warn("[INFO] 没有任何一类遮挡物存在 ≥2 个不同厚度，跳过 loss_by_thickness.png"
             "（提示词要求：仅当 dielectric 类里有 ≥2 个不同厚度时生成）。")
        return False

    category = "dielectric" if "dielectric" in qualified else max(
        qualified, key=lambda name: (len({row["thickness_mm"] for row in qualified[name]}),
                                     len(qualified[name])))
    rows = sorted(qualified[category], key=lambda row: row["thickness_mm"])

    x = np.array([row["thickness_mm"] for row in rows], dtype=float)
    y = np.array([row["loss_db"] for row in rows], dtype=float)
    yerr = np.array([row["sigma_loss"] for row in rows], dtype=float)
    slope, intercept = np.polyfit(x, y, 1)

    fig, ax = plt.subplots(figsize=(6, 4))
    color = CATEGORY_COLORS.get(category, "#4C72B0")
    ax.errorbar(x, y, yerr=yerr, fmt="o", color=color, ecolor="#555555",
                elinewidth=1.0, capsize=3, markersize=6, label="measurement")
    xs = np.linspace(float(x.min()) * 0.9, float(x.max()) * 1.1, 50)
    ax.plot(xs, slope * xs + intercept, "--", color="#C44E52",
            label=f"{labels['slope']}: {slope:.3f} dB/mm")

    for row in rows:
        ax.annotate(f"C{row['case_id']} {row['label']}",
                    (row["thickness_mm"], row["loss_db"]),
                    textcoords="offset points", xytext=(6, 6), fontsize=8)

    ax.set_xlabel(labels["thickness"])
    ax.set_ylabel(labels["loss"])
    ax.set_title(labels["thickness_title"].format(
        cat=CATEGORY_ZH.get(category, category), date=day))
    ax.set_ylim(float((y - yerr).min()) - 0.6, float((y + yerr).max()) + 0.9)
    ax.grid(linestyle=":", alpha=0.45)
    ax.legend(fontsize=8)

    fig.tight_layout()
    fig.savefig(path, dpi=300, facecolor="white")
    plt.close(fig)
    print(f"[INFO] loss_by_thickness.png 使用 {category} 类的 "
          f"{len({row['thickness_mm'] for row in rows})} 个厚度点，"
          f"拟合斜率 {slope:.3f} dB/mm。")
    return True


# ---------------------------------------------------------------------------
# 输出与命令行
# ---------------------------------------------------------------------------

SUMMARY_COLUMNS = ["case_id", "label", "material", "thickness_mm", "category",
                   "n", "median", "stdev", "loss_db", "sigma_loss"]
COMPARE_COLUMNS = ["case_id", "label", "category", "loss_measured",
                   "loss_ref_low", "loss_ref_high", "verdict"]


def print_report(summary: List[dict], compare: List[dict], stats: Dict[str, dict],
                 drift: float, base_median: float, sigma_base: float,
                 invalid: int, outlier_report: List[dict],
                 files: Dict[str, str]) -> None:
    print("=" * 74)
    print(" 遮挡穿透损耗分析结果")
    print("=" * 74)
    print(f"基准：首次 {stats['first_base']['median']:.2f} dBm（n={stats['first_base']['n']}），"
          f"末次 {stats['last_base']['median']:.2f} dBm（n={stats['last_base']['n']}）")
    print(f"      基准中位数 {base_median:.2f} dBm，σ_base={sigma_base:.2f} dB，"
          f"漂移 {drift:+.2f} dB（判据 ±{DRIFT_LIMIT_DB:g} dB，通过）")
    print(f"清洗：空 rssi {invalid} 行（采集失败），离群剔除 "
          f"{sum(item['n_out'] for item in outlier_report)} 行（> {MAD_K:g}×MAD）")
    for item in outlier_report:
        if item["n_out"]:
            print(f"  round {item['round_idx']:>2} C{item['case_id']} {item['role']:<10} "
                  f"剔除 {item['n_out']}/{item['n_in']}（MAD={item['mad']:.2f}）")

    print("\n各遮挡物（loss = 基准中位数 - 该测点中位数）：")
    print(f"  {'case':<6}{'label':<9}{'category':<11}{'厚度mm':>7}"
          f"{'n':>5}{'中位dBm':>10}{'loss dB':>10}{'σ dB':>7}  判定")
    verdict_by_case = {row["case_id"]: row["verdict"] for row in compare}
    for row in summary:
        verdict = verdict_by_case.get(row["case_id"], "?")
        print(f"  C{row['case_id']:<5}{row['label']:<9}{row['category']:<11}"
              f"{row['thickness_mm']:>7g}{row['n']:>5}{row['median']:>10.2f}"
              f"{row['loss_db']:>10.2f}{row['sigma_loss']:>7.2f}  {verdict}")

    worst = max(summary, key=lambda row: row["loss_db"])
    best = min(summary, key=lambda row: row["loss_db"])
    print(f"\n损耗最大：C{worst['case_id']} {worst['label']}（{worst['material']}）"
          f"= {worst['loss_db']:.2f} ± {worst['sigma_loss']:.2f} dB")
    print(f"损耗最小：C{best['case_id']} {best['label']}（{best['material']}）"
          f"= {best['loss_db']:.2f} ± {best['sigma_loss']:.2f} dB")

    deviations = []
    for row in compare:
        measured = row["loss_measured"]
        if measured > row["loss_ref_high"]:
            deviations.append((measured - row["loss_ref_high"], row))
        elif measured < row["loss_ref_low"]:
            deviations.append((row["loss_ref_low"] - measured, row))
    if deviations:
        gap, row = max(deviations, key=lambda item: item[0])
        print(f"与参考量级偏差最大：C{row['case_id']} {row['label']}"
              f"（{row['category']}，实测 {row['loss_measured']:.2f} dB，"
              f"参考 {row['loss_ref_low']:g}-{row['loss_ref_high']:g} dB，"
              f"偏差 {gap:.2f} dB，判定 {row['verdict']}）")
    else:
        print("所有测点都落在参考量级的 ±3 dB 容差内。")

    print("\n输出文件：")
    for title, path in files.items():
        print(f"  {title}: {path}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="analyze.py",
        description="计算遮挡穿透损耗并出图（需要 pandas + numpy + matplotlib）。",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--input", default=str(SCRIPT_DIR / "data" / "raw.csv"),
                        help="capture.py 输出的原始数据")
    parser.add_argument("--materials", default=str(SCRIPT_DIR / "data" / "materials.csv"),
                        help="手工材质表")
    parser.add_argument("--outdir", default=str(SCRIPT_DIR / "data"),
                        help="summary.csv / compare_theory.csv 的输出目录")
    parser.add_argument("--figdir", default=str(SCRIPT_DIR / "figures"),
                        help="图片输出目录")
    parser.add_argument("--date", default=None, help="图标题里的日期，默认今天")
    parser.add_argument("--lang", choices=("zh", "en"), default="zh",
                        help="图内文字语言；找不到中文字体时自动退回 en")
    parser.add_argument("--no-plots", action="store_true",
                        help="只算表、不出图（调试用）")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    day = args.date or date.today().isoformat()

    try:
        pd, np, matplotlib = import_stack()
        matplotlib.use("Agg")                 # 无界面后端，避免弹窗阻塞

        raw_path = Path(args.input).expanduser()
        frame, invalid = load_raw(pd, raw_path)
        raw_cases = {
            int(case): str(label).strip()
            for case, label in frame.groupby("case_id")["label"].first().items()
            if pd.notna(label)
        }
        materials = load_materials(pd, Path(args.materials).expanduser(), raw_cases)

        cleaned, outlier_report = remove_outliers(pd, frame)
        stats, drift = base_statistics(pd, cleaned)
        ensure_drift_ok(stats, drift)         # 超限立即终止，绝不照算出图

        summary, base_median, sigma_base = compute_summary(pd, cleaned, materials, stats)
        compare = compare_theory(summary)

        outdir = Path(args.outdir).expanduser()
        files = {
            "summary.csv": str(outdir / "summary.csv"),
            "compare_theory.csv": str(outdir / "compare_theory.csv"),
        }
        write_csv_rows(outdir / "summary.csv", SUMMARY_COLUMNS, summary,
                       decimals=("median", "stdev", "loss_db", "sigma_loss"),
                       compact=("thickness_mm",))
        write_csv_rows(outdir / "compare_theory.csv", COMPARE_COLUMNS, compare,
                       decimals=("loss_measured", "loss_ref_low", "loss_ref_high"))

        if args.no_plots:
            print("[INFO] --no-plots：只写出 CSV，未出图。")
        else:
            plt = importlib.import_module("matplotlib.pyplot")
            if args.lang == "en":
                font_ok = setup_style(plt, False)
                labels = LABELS["en"]
            else:
                font_ok = setup_style(plt, True)
                labels = LABELS["zh"] if font_ok else LABELS["en"]

            figdir = Path(args.figdir).expanduser()
            figdir.mkdir(parents=True, exist_ok=True)
            material_png = figdir / "loss_by_material.png"
            drift_png = figdir / "baseline_drift.png"
            plot_loss_by_material(plt, np, summary, material_png, labels, font_ok, day)
            plot_baseline_drift(plt, np, stats, drift, drift_png, labels, day)
            files["loss_by_material.png"] = str(material_png)
            files["baseline_drift.png"] = str(drift_png)
            thickness_png = figdir / "loss_by_thickness.png"
            if plot_loss_by_thickness(plt, np, summary, thickness_png, labels, day):
                files["loss_by_thickness.png"] = str(thickness_png)

        print_report(summary, compare, stats, drift, base_median, sigma_base,
                     invalid, outlier_report, files)
        return 0
    except AnalysisError as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())