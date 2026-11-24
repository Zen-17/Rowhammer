"""Phase 3 figures.  Geometry and color come only from ``style.py``.

Figure 2(a) is the M0 benign panel.  Panels (b) and (c), and figures 3-8, are
refused until their CSVs exist.  Real-threshold and 1/16 data are never drawn
on one pair of axes.
"""
from __future__ import annotations

import csv
import json
from collections import defaultdict
from pathlib import Path
from typing import Dict, List

import matplotlib.pyplot as plt
import numpy as np

from phase3_exp.plot import style

CAPTION_2A = (
    "Fig. 2(a). Hottest-row share f_max of the worst complete 1024-ACT window "
    "under no attack. Streaming and reuse are the two bars of each workload. "
    "The axis breaks above 0.07 so the W4 reuse bar can continue to its measured "
    "value. Dashed line is f_th = 1/16; dotted line is the G1 reference 0.02. "
    "Partial tails are omitted. Attack panels are not drawn."
)


def _read_csv(path: Path) -> List[dict]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def benign_points(rows: List[dict]) -> Dict[str, Dict[str, float]]:
    """One f_max and one f_p99 per workload: the worst bank in the profile."""
    grouped = defaultdict(list)
    for row in rows:
        if row.get("status", "SUCCESS") == "FAILED":
            continue
        if str(row.get("partial", "")).lower() == "true":
            continue
        grouped[row["workload"]].append(row)
    missing = [name for name in style.WORKLOAD_ORDER if name not in grouped]
    if missing:
        raise FileNotFoundError(f"fig2(a) refused, missing workloads: {missing}")
    points = {}
    for name, items in grouped.items():
        points[name] = {
            "f_max": max(float(item["f_max"]) for item in items),
            "f_p99": max(float(item["f_p99"]) for item in items),
        }
    return points


def _draw_panel(axis, points: Dict[str, Dict[str, float]], label: str) -> dict:
    names = [name for name in style.WORKLOAD_ORDER if name in points]
    x = np.arange(len(names))
    width = 0.36
    f_p99 = [points[name]["f_p99"] for name in names]
    f_max = [points[name]["f_max"] for name in names]
    axis.bar(x - width / 2, f_p99, width, label="f_p99", color="#0072B2",
             edgecolor="white", linewidth=0.4, zorder=2)
    axis.bar(x + width / 2, f_max, width, label="f_max", color="#D55E00",
             edgecolor="white", linewidth=0.4, zorder=2)
    guides = style.reference_lines(axis)
    style.panel_label(axis, label)
    axis.set_xticks(x)
    axis.set_xticklabels(names)
    axis.set_ylabel("Row activation share")
    axis.set_xlabel("Workload")
    peak = max(f_p99 + f_max)
    axis.set_ylim(0.0, max(0.08, peak * 1.18))
    axis.legend(frameon=False, loc="upper right")
    return guides


def _mode_series(rows: List[dict]):
    order = ["streaming", "reuse"]
    present = []
    points = {}
    for mode in order:
        selected = [row for row in rows if row.get("access_mode") == mode]
        if not selected:
            continue
        present.append(mode)
        points[mode] = benign_points(selected)
    names = [name for name in style.WORKLOAD_ORDER if all(name in points[mode] for mode in present)]
    palette = {"streaming": "#0072B2", "reuse": "#D55E00"}
    labels = {"streaming": "Streaming", "reuse": "Reuse"}
    series = [
        (labels[mode], [points[mode][name]["f_max"] for name in names], palette[mode])
        for mode in present
    ]
    return names, series, points


def _bars(axis, names, series, width=0.36):
    x = np.arange(len(names))
    handles = []
    for index, (label, values, color) in enumerate(series):
        offset = (index - (len(series) - 1) / 2.0) * width
        handle = axis.bar(
            x + offset, values, width=width * 0.92, color=color,
            edgecolor="white", linewidth=0.35, label=label, zorder=3,
        )
        handles.append(handle)
    axis.set_xlim(-0.55, len(names) - 0.45)
    return handles


def _break_marks(axis, top: float = 1.075) -> None:
    """One pair of slashes across the left spine, in the gap above it."""
    for shift in (-0.007, 0.007):
        axis.plot(
            (-0.010 + shift, 0.010 + shift),
            (1.015, top),
            transform=axis.transAxes, color="#000000", clip_on=False,
            linewidth=0.8, solid_capstyle="butt", zorder=5,
        )


def _figure_modes(rows: List[dict]):
    """One comparison.  The axis breaks where no bar falls, so W4 reuse is not clipped."""
    names, series, points = _mode_series(rows)
    fig, (top, bottom) = plt.subplots(
        2, 1, sharex=True, figsize=(style.DOUBLE[0], 2.85),
        gridspec_kw={"height_ratios": [0.85, 2.55], "hspace": 0.07},
    )
    _bars(top, names, series)
    _bars(bottom, names, series)
    bottom.set_ylim(0.0, 0.078)
    top.set_ylim(0.105, 0.158)
    bottom.set_yticks([0.0, 0.02, 0.04, 0.06])
    top.set_yticks([0.12, 0.14])
    bottom.set_ylabel("Row activation share")
    bottom.set_xlabel("Workload")
    bottom.set_xticks(np.arange(len(names)))
    bottom.set_xticklabels(names)
    top.tick_params(axis="x", which="both", bottom=False, labelbottom=False)
    top.spines["bottom"].set_visible(False)
    bottom.axhline(style.F_TH, color="#000000", linestyle="--", linewidth=0.7, zorder=1)
    bottom.axhline(style.G1_REFERENCE, color="#666666", linestyle=":", linewidth=0.7, zorder=1)
    bottom.text(
        0.02, style.G1_REFERENCE, " G1 = 0.02", fontsize=6, color="#666666",
        ha="left", va="bottom",
    )
    bottom.text(
        len(names) - 0.55, style.F_TH, "f_th = 0.0625  ", fontsize=6, color="#000000",
        ha="right", va="bottom",
    )
    _break_marks(bottom)
    if "reuse" in points and "W4" in names:
        value = points["reuse"]["W4"]["f_max"]
        slot = names.index("W4") + (len(series) - 1) / 2.0 * 0.36
        top.annotate(
            f"{value:.3f}",
            xy=(slot, value), xytext=(0, 1.5), textcoords="offset points",
            ha="center", va="bottom", fontsize=6.5, color="#D55E00",
            annotation_clip=False,
        )
    top.legend(frameon=False, loc="upper left", ncol=2, borderaxespad=0.2)
    fig.align_ylabels([top, bottom])
    return fig, {"f_th": style.F_TH, "g1": style.G1_REFERENCE}


def _merge_manifest(figures: Path, entries: List[dict]) -> None:
    path = figures / "manifest.json"
    existing: List[dict] = []
    if path.exists():
        existing = json.loads(path.read_text())
    names = {entry["figure"] for entry in entries}
    kept = [entry for entry in existing if entry.get("figure") not in names]
    style.write_manifest(figures, kept + entries)


def figure_2a(profile_csv: Path, figures: Path) -> dict:
    rows = _read_csv(profile_csv)
    manifest: List[dict] = []
    has_modes = any(row.get("access_mode") for row in rows)
    if has_modes:
        fig, guides = _figure_modes(rows)
        sample_count = 2 * len(style.WORKLOAD_ORDER)
        aggregation = "worst complete-window f_max, streaming beside reuse"
    else:
        points = benign_points(rows)
        fig, axis = plt.subplots(figsize=style.SINGLE)
        guides = _draw_panel(axis, points, "(a)")
        fig.tight_layout()
        sample_count = len(points)
        aggregation = "worst bank f_max and f_p99"
    style.save_figure(
        fig, "fig2a_e1_benign", figures, manifest, [profile_csv],
        {"panel": "a", "attack": "NO_ATK", "guides": guides},
        sample_count=sample_count, aggregation=aggregation,
        caption=CAPTION_2A,
    )
    _merge_manifest(figures, manifest)
    return manifest[0]


CAPTION_2 = (
    "Fig. 2. (a) Hottest-row share f_max of the worst complete 1024-ACT window "
    "under no attack. Streaming and reuse are adjacent bars. The axis breaks above "
    "0.07 so the W4 reuse bar stays visible. The dashed line is f_th = 0.0625 and "
    "the dotted line is 0.02; neither is in the legend. (b) Cumulative flips for "
    "A1 x NONE at the real 14.5K threshold. (c) The same curve at the 1/16 "
    "threshold, drawn on its own axes."
)


def _curve(axis, rows: List[dict], scale: float, xlabel: str, panel: str) -> int:
    selected = [row for row in rows if abs(float(row["scale"]) - scale) < 1e-9]
    selected.sort(key=lambda row: int(row["act_ordinal"]))
    xs = [0] + [int(row["act_ordinal"]) for row in selected]
    ys = [0] + [int(row["cumulative_flips"]) for row in selected]
    axis.step(
        xs, ys, where="post", color=style.SCHEME_COLORS["NONE"],
        label="A1 × NONE", zorder=3,
    )
    axis.plot(xs, ys, linestyle="none", marker="o", color=style.SCHEME_COLORS["NONE"],
              markersize=3.0, zorder=4)
    style.panel_label(axis, panel)
    axis.set_xlabel(xlabel)
    axis.set_ylabel(f"{panel} Cumulative flips")
    axis.set_xlim(left=0)
    axis.set_ylim(0.0, max(ys + [1]) * 1.35)
    axis.legend(frameon=False, loc="upper left")
    return len(selected)


def figure_2(profile_csv: Path, flips_csv: Path, figures: Path) -> dict:
    """Three panels.  Real 14.5K and 1/16 flips never share an axis."""
    profile = _read_csv(profile_csv)
    flips = _read_csv(flips_csv)
    if not any(abs(float(row["scale"]) - 1.0) < 1e-9 for row in flips):
        raise FileNotFoundError("fig2(b) needs the real 14.5K A1 x NONE curve")
    if not any(abs(float(row["scale"]) - 0.0625) < 1e-9 for row in flips):
        raise FileNotFoundError("fig2(c) needs the 1/16 A1 x NONE curve")
    names, series, points = _mode_series(profile)
    fig = plt.figure(figsize=(7.16, 4.70))
    outer = fig.add_gridspec(
        2, 1, height_ratios=[1.72, 1.05], hspace=0.46,
        left=0.11, right=0.985, top=0.94, bottom=0.09,
    )
    broken = outer[0].subgridspec(2, 1, height_ratios=[0.78, 2.05], hspace=0.28)
    top = fig.add_subplot(broken[0])
    bottom = fig.add_subplot(broken[1], sharex=top)
    _bars(top, names, series)
    _bars(bottom, names, series)
    bottom.set_ylim(0.0, 0.078)
    top.set_ylim(0.105, 0.172)
    bottom.set_yticks([0.0, 0.02, 0.04, 0.06])
    top.set_yticks([0.12, 0.14])
    bottom.set_ylabel("(a) Row activation share")
    bottom.set_xlabel("Workload")
    bottom.set_xticks(np.arange(len(names)))
    bottom.set_xticklabels(names)
    top.tick_params(axis="x", which="both", bottom=False, labelbottom=False)
    top.spines["bottom"].set_visible(False)
    bottom.axhline(style.F_TH, color="#000000", linestyle="--", linewidth=0.7, zorder=1)
    bottom.axhline(style.G1_REFERENCE, color="#666666", linestyle=":", linewidth=0.7, zorder=1)
    bottom.text(0.02, style.G1_REFERENCE, " G1 = 0.02", fontsize=6, color="#666666",
                ha="left", va="bottom")
    bottom.text(len(names) - 0.55, style.F_TH, "f_th = 0.0625  ", fontsize=6,
                color="#000000", ha="right", va="bottom")
    _break_marks(bottom, top=1.28)
    if "reuse" in points and "W4" in names:
        value = points["reuse"]["W4"]["f_max"]
        slot = names.index("W4") + (len(series) - 1) / 2.0 * 0.36
        top.annotate(
            f"{value:.3f}", xy=(slot, value), xytext=(0, 1.5),
            textcoords="offset points", ha="center", va="bottom", fontsize=6.5,
            color="#D55E00", annotation_clip=False,
        )
    top.legend(frameon=False, loc="upper left", ncol=2, borderaxespad=0.2)
    style.panel_label(top, "(a)")
    curves = outer[1].subgridspec(1, 2, wspace=0.34)
    real_n = _curve(
        fig.add_subplot(curves[0]), flips, 1.0,
        "Attack activations (real 14.5K)", "(b)",
    )
    scaled_n = _curve(
        fig.add_subplot(curves[1]), flips, 0.0625,
        "Attack activations (1/16 scale)", "(c)",
    )
    manifest: List[dict] = []
    style.save_figure(
        fig, "fig2_e1_characterization", figures, manifest, [profile_csv, flips_csv],
        {"panels": ["a", "b", "c"], "attack": "A1 x NONE", "f_th": style.F_TH},
        sample_count=real_n + scaled_n,
        aggregation="one A1 x NONE trace per threshold; worst complete-window f_max",
        caption=CAPTION_2,
    )
    _merge_manifest(figures, manifest)
    return manifest[0]


def _require_csv(path: Path, figure: str) -> None:
    if not path.exists():
        raise FileNotFoundError(f"{figure} is not drawn; missing {path.name}")


def figure_2_attack(real_csv: Path, scaled_csv: Path, figures: Path) -> None:
    """Two panels.  The 14.5K curve and the 1/16 curve never share an axis."""
    _require_csv(real_csv, "fig2(b)")
    _require_csv(scaled_csv, "fig2(c)")
    raise NotImplementedError("attack curves are drawn only after M1 produces both CSVs")


def figure_3(csv_path: Path, figures: Path) -> dict:
    """One A1 timeline per scheme.  The 1/16 panel and the 14.5K panel do not share an axis."""
    rows = [
        row for row in _read_csv(csv_path)
        if row.get("status", "SUCCESS") == "SUCCESS" and row.get("attack") == "A1"
        and row.get("index") == "0" and row.get("act")
    ]
    schemes = ["NONE", "PARA", "FIX", "ADA", "OURS"]
    panels = (
        (0.0625, "1/16 threshold", "Attack activations (1/16)"),
        (1.0, "Real 14.5K threshold", "Attack activations (14.5K)"),
    )
    fig = plt.figure(figsize=(7.16, 2.7))
    drawn = 0
    for index, (scale, title, xlabel) in enumerate(panels):
        axis = fig.add_subplot(1, 2, index + 1)
        for scheme in schemes:
            series = [
                row for row in rows
                if row["scheme"] == scheme and abs(float(row["scale"]) - scale) < 1e-9
                and str(row.get("cum_flips", "")).strip().lstrip("-").isdigit()
            ]
            series.sort(key=lambda row: int(row["act"]))
            if not series:
                continue
            axis.plot(
                [int(row["act"]) for row in series],
                [int(row["cum_flips"]) for row in series],
                color=style.scheme_color(scheme), label=scheme,
            )
            drawn += len(series)
        axis.set_title(title, fontsize=8)
        axis.set_xlabel(xlabel)
        axis.set_ylabel("Cumulative flips")
        axis.set_ylim(bottom=0)
        axis.legend(frameon=False, ncol=1, loc="upper left", borderaxespad=0.2)
        style.panel_label(axis, "(a)" if index == 0 else "(b)")
    fig.subplots_adjust(wspace=0.38, left=0.08, right=0.98, top=0.86, bottom=0.18)
    manifest: List[dict] = []
    style.save_figure(
        fig, "fig3_e3_attack_timeline", figures, manifest, [csv_path],
        {"attack": "A1", "index": 0, "panels": ["1/16", "14.5K"]},
        sample_count=drawn,
        aggregation="one A1 trace per scheme and threshold; other attacks stay in the CSV",
        caption=(
            "Fig. 3. Cumulative flips under A1 on W2. "
            "The 1/16 threshold and the real 14.5K threshold are separate panels. "
            "FIX and ADA stay at zero flips. OURS is the preview without CE feedback."
        ),
    )
    _merge_manifest(figures, manifest)
    return manifest[0]


def figure_4(csv_path: Path, figures: Path) -> dict:
    """Worst-attack MTTF over region size and c.  The design point is n=64, c=2."""
    rows = _read_csv(csv_path)
    ns = [16, 32, 64, 128, 256, 16384]
    cs = [1.5, 2.0, 2.5, 3.0]
    schemes = ["FIX", "ADA", "OURS"]
    margins = [1.5, 1.1]
    grouped = {}
    for row in rows:
        if row.get("status", "SUCCESS") == "FAILED":
            continue
        key = (int(row["n"]), float(row["c"]), float(row["g"]), row["scheme"])
        value = float(row["mttf_flip_mean"])
        grouped[key] = value if key not in grouped else min(grouped[key], value)
    fig = plt.figure(figsize=(7.16, 4.55))
    grid = fig.add_gridspec(
        2, 3, hspace=0.55, wspace=0.28, left=0.10, right=0.88, top=0.90, bottom=0.12,
    )
    image = None
    for row_index, margin in enumerate(margins):
        for col_index, scheme in enumerate(schemes):
            axis = fig.add_subplot(grid[row_index, col_index])
            values = np.full((len(ns), len(cs)), np.nan)
            for y, n in enumerate(ns):
                for x, c in enumerate(cs):
                    values[y, x] = grouped[(n, c, margin, scheme)]
            image = axis.imshow(
                np.log10(values), cmap=style.HEATMAP_CMAP, vmin=4.0,
                vmax=np.log10(614400.0), origin="lower", aspect="auto",
            )
            axis.scatter(
                [cs.index(2.0)], [ns.index(64)], s=28, facecolors="none",
                edgecolors="#000000", linewidths=0.8, zorder=4,
            )
            if scheme == "FIX" and margin == 1.5:
                axis.annotate(
                    "n=64, c=2", xy=(cs.index(2.0), ns.index(64)),
                    xytext=(10, 8), textcoords="offset points", fontsize=6, color="#000000",
                )
            style.panel_label(axis, f"({chr(ord('a') + row_index * 3 + col_index)})")
            axis.set_xticks(range(len(cs)))
            axis.set_xticklabels(["1.5", "2", "2.5", "3"])
            axis.set_yticks(range(len(ns)))
            axis.set_yticklabels(["16", "32", "64", "128", "256", "16384"] if col_index == 0 else [])
            axis.set_title(f"{scheme}, g={margin:g}", fontsize=8)
            if row_index == 1:
                axis.set_xlabel("Swap margin c")
            if col_index == 0:
                axis.set_ylabel("Region size n")
    bar = fig.colorbar(image, ax=fig.axes, fraction=0.025, pad=0.02)
    bar.set_label("log10 MTTF_flip, worst of A1-A4")
    manifest: List[dict] = []
    style.save_figure(
        fig, "fig4_e2_mttf_heatmap", figures, manifest, [csv_path],
        {"design_point": {"n": 64, "c": 2}, "value": "min MTTF_flip over A1-A4"},
        sample_count=len(rows),
        aggregation="minimum MTTF_flip across A1-A4, 1000 trials",
        caption=(
            "Fig. 4. Monte Carlo MTTF to the first flip. Color is log10 of the worst "
            "attack. The open circle is the design point n=64, c=2. "
            "g=1.5 is FIX and ADA; g=1.1 is the OURS margin."
        ),
    )
    _merge_manifest(figures, manifest)
    return manifest[0]


def figure_5(csv_path: Path, figures: Path) -> dict:
    """HBM2 only.  HBM3 is absent until its ini exists, and it does not share this axis."""
    rows = [
        row for row in _read_csv(csv_path)
        if row.get("status") == "SUCCESS" and row.get("device") == "HBM2" and row.get("kind") == "hc_scan"
    ]
    schemes = ["NONE", "PARA", "FIX", "ADA", "OURS"]
    fig = plt.figure(figsize=(7.16, 2.55))
    axis = fig.add_subplot(1, 1, 1)
    drawn = 0
    y_max = 1.0
    for scheme in schemes:
        series = sorted(
            [row for row in rows if row["scheme"] == scheme and row.get("overhead_pct")],
            key=lambda row: float(row["hc"]),
        )
        if not series:
            continue
        ys = [float(row["overhead_pct"]) for row in series]
        y_max = max(y_max, max(ys))
        axis.plot(
            [float(row["hc"]) for row in series], ys,
            color=style.scheme_color(scheme), marker="o", label=scheme,
        )
        drawn += len(series)
    # r1 = floor((HC/g)/(c*n)).  r1 reaches 10 at HC = 10*c*n*g.
    boundaries = ((10 * 2 * 64 * 1.5, "r1=10, g=1.5"), (10 * 2 * 64 * 1.1, "r1=10, g=1.1"))
    for value, _label in boundaries:
        axis.axvline(value, color="#000000", linestyle="--", linewidth=0.6)
    axis.text(
        5200, y_max * 0.78,
        "r1 = 10 at HC 1920 (g=1.5)\nr1 = 10 at HC 1408 (g=1.1)",
        fontsize=6, color="#000000", ha="left", va="top",
    )
    axis.set_xlim(700, 16000)
    axis.set_ylim(0, y_max * 1.18)
    axis.set_xlabel("HC first,min (HBM2)")
    axis.set_ylabel("Cycle overhead vs NONE (%)")
    axis.legend(frameon=False, ncol=5, loc="upper right", borderaxespad=0.2)
    style.panel_label(axis, "(a)")
    fig.subplots_adjust(left=0.10, right=0.98, top=0.90, bottom=0.18)
    manifest: List[dict] = []
    style.save_figure(
        fig, "fig5_e4_overhead_vs_hc", figures, manifest, [csv_path],
        {"device": "HBM2", "workload": "W2", "r1_eq_10": {"g1.5": 1920, "g1.1": 1408}},
        sample_count=drawn,
        aggregation="mean is not pooled; each point is the W2 scan run at that HC",
        caption=(
            "Fig. 5. W2 cycle overhead against the no-defense run as the flip threshold falls. "
            "Only HBM2 is drawn. Dashed lines are the HC values where r1 reaches 10, "
            "from r1 = floor((HC/g)/(c*n)) with c=2 and n=64."
        ),
    )
    _merge_manifest(figures, manifest)
    return manifest[0]


def figure_6(mttf_csv: Path, campaign_csv: Path, figures: Path) -> None:
    _require_csv(mttf_csv, "fig6")
    _require_csv(campaign_csv, "fig6")
    raise NotImplementedError("fig6 waits for E2 and E5")


def figure_7(csv_path: Path, figures: Path) -> None:
    _require_csv(csv_path, "fig7")
    raise NotImplementedError("fig7 waits for E5_campaign_summary.csv")


def figure_8(csv_path: Path, figures: Path) -> None:
    _require_csv(csv_path, "fig8")
    raise NotImplementedError("fig8 waits for E7_sensitivity.csv")
