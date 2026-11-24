"""M2-R figures.  Does not rewrite fig2-fig5.

Protected cells have no observed flip, so figure 4r colors the disturbance
upper bound.  The NONE panel uses the observed flip rate.
"""
from __future__ import annotations

import csv
import json
from pathlib import Path
from typing import List

import matplotlib.pyplot as plt
import numpy as np

from phase3_exp.plot import style

NS = [16, 32, 64, 128, 256, 16384]
CS = [1.5, 2.0, 2.5, 3.0]
ATTACKS = ["A1", "A2", "A3", "A4"]
ATTACK_COLORS = ["#000000", "#0072B2", "#009E73", "#D55E00"]


def _read(path: Path) -> List[dict]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def _merge(figures: Path, entries: List[dict]) -> None:
    path = figures / "manifest.json"
    existing = json.loads(path.read_text()) if path.exists() else []
    names = {entry["figure"] for entry in entries}
    kept = [entry for entry in existing if entry.get("figure") not in names]
    style.write_manifest(figures, kept + entries)


def figure_4r(csv_path: Path, figures: Path) -> None:
    rows = [row for row in _read(csv_path) if row["scheme"] != "NONE" and row["status"] == "SUCCESS"]
    grouped = {}
    for row in rows:
        key = (int(row["n"]), float(row["c"]), float(row["g"]), row["scheme"])
        value = float(row["max_ratio_max"])
        grouped[key] = value if key not in grouped else max(grouped[key], value)
    panels = [(1.5, "FIX"), (1.5, "ADA"), (1.5, "OURS"), (1.1, "FIX"), (1.1, "ADA"), (1.1, "OURS")]
    fig = plt.figure(figsize=(7.16, 4.55))
    grid = fig.add_gridspec(2, 3, hspace=0.62, wspace=0.28, left=0.10, right=0.88, top=0.90, bottom=0.12)
    image = None
    for index, (margin, scheme) in enumerate(panels):
        axis = fig.add_subplot(grid[index // 3, index % 3])
        values = np.full((len(NS), len(CS)), np.nan)
        for y, n in enumerate(NS):
            for x, c in enumerate(CS):
                values[y, x] = grouped[(n, c, margin, scheme)]
        image = axis.imshow(values, cmap=style.HEATMAP_CMAP, vmin=0.0, vmax=1.0, origin="lower", aspect="auto")
        axis.scatter([CS.index(2.0)], [NS.index(64)], s=28, facecolors="none", edgecolors="#000000", linewidths=0.8, zorder=4)
        if scheme == "FIX" and margin == 1.5:
            axis.annotate("n=64, c=2", xy=(CS.index(2.0), NS.index(64)), xytext=(8, 7),
                          textcoords="offset points", fontsize=6, color="#000000")
        style.panel_label(axis, f"({chr(ord('a') + index)})")
        axis.set_xticks(range(len(CS)))
        axis.set_xticklabels(["1.5", "2", "2.5", "3"])
        axis.set_yticks(range(len(NS)))
        axis.set_yticklabels(["16", "32", "64", "128", "256", "16384"] if index % 3 == 0 else [])
        axis.set_title(f"{scheme}, g={margin:g}", fontsize=8)
        if index >= 3:
            axis.set_xlabel("Swap margin c")
        if index % 3 == 0:
            axis.set_ylabel("Region size n")
    bar = fig.colorbar(image, ax=fig.axes, fraction=0.025, pad=0.02)
    bar.set_label("Worst-attack D bound / threshold")
    manifest: List[dict] = []
    style.save_figure(
        fig, "fig4r_e2_disturbance_margin", figures, manifest, [csv_path],
        {"value": "max max_ratio_max over A1-A4", "p_flip": 0, "design_point": {"n": 64, "c": 2}},
        sample_count=len(rows),
        aggregation="maximum epoch upper bound across A1-A4, 200 trials, P_flip=0",
        caption=(
            "Fig. 4r. Upper bound on D(v) divided by the first-flip threshold. "
            "Every protected trial is right-censored, so the color is not a time-to-flip. "
            "The open circle is n=64, c=2. A bound of 1 means one swap epoch can reach the threshold."
        ),
    )
    _merge(figures, manifest)


def figure_4r_none(csv_path: Path, figures: Path) -> None:
    rows = [row for row in _read(csv_path) if row["scheme"] == "NONE" and row["status"] == "SUCCESS"]
    ns = [16, 64, 256, 16384]
    fig, axis = plt.subplots(figsize=style.DOUBLE)
    width = 0.18
    for offset, attack, color in zip((-1.5, -0.5, 0.5, 1.5), ATTACKS, ATTACK_COLORS):
        xs, ys, lows, highs = [], [], [], []
        for index, n in enumerate(ns):
            match = next(row for row in rows if int(row["n"]) == n and row["attack"] == attack)
            p = float(match["p_flip"])
            xs.append(index + offset * width)
            ys.append(p)
            lows.append(max(0.0, p - float(match["p_flip_low"])))
            highs.append(max(0.0, float(match["p_flip_high"]) - p))
        axis.bar(xs, ys, width=width, color=color, label=attack, zorder=2)
        axis.errorbar(xs, ys, yerr=[lows, highs], fmt="none", ecolor="#000000", elinewidth=0.6, capsize=1.5, zorder=3)
    axis.set_xticks(range(len(ns)))
    axis.set_xticklabels(["16", "64", "256", "16384"])
    axis.set_ylim(0, 1.08)
    axis.set_xlabel("Region size n")
    axis.set_ylabel("P_flip within one refresh window")
    axis.legend(frameon=False, ncol=4, loc="upper center", bbox_to_anchor=(0.5, 1.16))
    style.panel_label(axis, "(a)")
    fig.subplots_adjust(left=0.10, right=0.98, top=0.82, bottom=0.18)
    manifest: List[dict] = []
    style.save_figure(
        fig, "fig4r_e2_none_pflip", figures, manifest, [csv_path],
        {"scheme": "NONE", "trials": 200, "interval": "Wilson 95%"},
        sample_count=len(rows),
        aggregation="flip count / 200, Wilson interval from harness.stats",
        caption=(
            "Fig. 4r none. Unprotected flip rate inside one refresh window. "
            "Bars are observed P_flip. Whiskers are the 95% Wilson interval. "
            "A missing bar is a right-censored cell, not a failure at the horizon."
        ),
    )
    _merge(figures, manifest)


def figure_3r(seed_csv: Path, a3_csv: Path, figures: Path) -> None:
    schemes = ["NONE", "FIX", "ADA", "OURS"]
    attacks = ["A1", "A2", "A4", "A3"]
    seeds = [row for row in _read(seed_csv) if row["status"] == "SUCCESS"]
    a3_rows = [row for row in _read(a3_csv) if row["status"] == "SUCCESS"]
    fig, axes = plt.subplots(1, 2, figsize=style.DOUBLE)
    width = 0.18
    for offset, scheme in zip((-1.5, -0.5, 0.5, 1.5), schemes):
        rates, peaks = [], []
        counts = []
        for attack in attacks:
            if attack == "A3":
                subset = [row for row in a3_rows if row["scheme"] == scheme]
            else:
                subset = [row for row in seeds if row["scheme"] == scheme and row["attack"] == attack]
            flipped = sum(1 for row in subset if int(row["injections"]) > 0)
            rates.append(flipped / len(subset))
            counts.append((flipped, len(subset)))
            peak_key = "window_peak_ratio" if attack == "A3" and "window_peak_ratio" in subset[0] else "peak_ratio"
            peaks.append(sum(float(row[peak_key]) for row in subset) / len(subset))
        xs = np.arange(len(attacks)) + offset * width
        color = style.scheme_color(scheme)
        axes[0].bar(xs, rates, width=width, color=color, label=scheme, zorder=2)
        axes[1].bar(xs, peaks, width=width, color=color, zorder=2)
        if scheme == "NONE":
            for x, rate, (flipped, total) in zip(xs, rates, counts):
                axes[0].text(x, max(rate, 0.02) + 0.04, f"{flipped}/{total}", ha="center", va="bottom", fontsize=6)
    for axis in axes:
        axis.set_xticks(range(len(attacks)))
        axis.set_xticklabels(["A1", "A2", "A4", "A3*"])
    axes[0].set_ylabel("Fraction of runs with an injection")
    axes[0].set_ylim(0, 1.28)
    axes[1].axhline(1.0, color="#000000", linestyle="--", linewidth=0.7, zorder=1)
    axes[1].text(0.98, 1.0, "threshold", transform=axes[1].get_yaxis_transform(),
                 va="bottom", ha="right", fontsize=6)
    axes[1].set_ylabel("Mean victim peak D / threshold")
    axes[1].set_ylim(0, 1.45)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, ncol=4, loc="upper center", bbox_to_anchor=(0.55, 1.04))
    style.panel_label(axes[0], "(a)")
    style.panel_label(axes[1], "(b)")
    fig.subplots_adjust(left=0.09, right=0.98, top=0.78, bottom=0.16, wspace=0.28)
    manifest: List[dict] = []
    style.save_figure(
        fig, "fig3r_e3_injections", figures, manifest, [seed_csv, a3_csv],
        {"refresh": "on", "workload": "W2", "hc": 14531, "a3": "one victim refresh period"},
        sample_count=len(seeds) + len(a3_rows),
        aggregation="A1/A2/A4: 4 seeds; A3*: one seed over 8192 REFs after the victim refresh",
        caption=(
            "Fig. 3r. Real-threshold W2 after the refresh fix. "
            "(a) Fraction of runs with at least one injection. Labels are the NONE counts. "
            "FIX, ADA, and OURS are 0/4 on A1, A2, and A4, and 0/1 on A3*. "
            "(b) Mean peak disturbance of the logical victim divided by its threshold. "
            "A3* is one victim-row refresh period, not the 1024-access definition. "
            "A2 injections land on other hammered rows, so a victim bar below the threshold can still have injections."
        ),
    )
    _merge(figures, manifest)


def figure_3s(csv_path: Path, figures: Path) -> None:
    rows = [row for row in _read(csv_path) if row["status"] == "SUCCESS" and row["attack"] == "A1"]
    schemes = ["NONE", "FIX", "ADA", "OURS"]
    panels = [(1147387685, "Seed where NONE injects"), (20261122, "Seed where NONE does not")]
    fig, axes = plt.subplots(1, 2, figsize=style.DOUBLE, sharey=True)
    width = 0.36
    for axis, (seed, title) in zip(axes, panels):
        for offset, accounting, hatch in ((-0.5, "model", ""), (0.5, "commands", "//")):
            peaks = []
            injections = []
            for scheme in schemes:
                match = next(row for row in rows if int(row["seed"]) == seed and row["scheme"] == scheme
                             and row["swap_accounting"] == accounting)
                peaks.append(float(match["peak_ratio"]))
                injections.append(int(match["injections"]))
            xs = np.arange(len(schemes)) + offset * width
            axis.bar(xs, peaks, width=width, color=[style.scheme_color(name) for name in schemes],
                     hatch=hatch, edgecolor="#000000", linewidth=0.3, zorder=2)
            for x, height, count in zip(xs, peaks, injections):
                if count:
                    axis.text(x, height + 0.04, str(count), ha="center", va="bottom", fontsize=6)
        axis.axhline(1.0, color="#000000", linestyle="--", linewidth=0.7, zorder=1)
        axis.set_xticks(range(len(schemes)))
        axis.set_xticklabels(schemes)
        axis.set_title(title, fontsize=8)
    axes[0].set_ylabel("Victim peak D / threshold")
    axes[0].set_ylim(0, 2.15)
    from matplotlib.patches import Patch
    fig.legend(
        [Patch(facecolor="#FFFFFF", edgecolor="#000000", label="Swap model"),
         Patch(facecolor="#FFFFFF", edgecolor="#000000", hatch="//", label="ACT only")],
        ["Swap model", "ACT only"], frameon=False, ncol=2, loc="upper center", bbox_to_anchor=(0.55, 1.04),
    )
    style.panel_label(axes[0], "(a)")
    style.panel_label(axes[1], "(b)")
    fig.subplots_adjust(left=0.09, right=0.98, top=0.76, bottom=0.16, wspace=0.16)
    manifest: List[dict] = []
    style.save_figure(
        fig, "fig3s_swap_sensitivity", figures, manifest, [csv_path],
        {"attack": "A1", "refresh": "on", "hc": 14531, "hatched": "command-only swap accounting"},
        sample_count=len(rows),
        aggregation="one run per scheme, seed, and swap accounting; label is the injection count when nonzero",
        caption=(
            "Fig. 3s. A1 sensitivity at the real threshold. Solid bars keep the v1 swap disturbance update. "
            "Hatched bars leave D unchanged at noteSwap and count only command-bus activations. "
            "The number above a bar is the injection count."
        ),
    )
    _merge(figures, manifest)


def figure_5r(csv_path: Path, figures: Path) -> None:
    rows = [row for row in _read(csv_path) if row["status"] == "SUCCESS"]
    schemes = ["PARA", "FIX", "ADA", "OURS"]
    workloads = ["W2", "GEMV", "DEC"]
    modes = [("streaming", "Streaming"), ("reuse", "Reuse")]
    fig, axes = plt.subplots(1, 2, figsize=style.DOUBLE, sharey=True)
    width = 0.18
    for axis, (mode, title) in zip(axes, modes):
        for offset, scheme in zip((-1.5, -0.5, 0.5, 1.5), schemes):
            ys = []
            for workload in workloads:
                subset = [
                    float(row["overhead_pct_v1"]) for row in rows
                    if row["access_mode"] == mode and row["scheme"] == scheme and row["workload"] == workload
                ]
                ys.append(sum(subset) / len(subset))
            xs = np.arange(len(workloads)) + offset * width
            axis.bar(xs, ys, width=width, color=style.scheme_color(scheme), label=scheme, zorder=2)
        axis.set_xticks(range(len(workloads)))
        axis.set_xticklabels(workloads)
        axis.set_title(title, fontsize=8)
    axes[0].set_ylabel("Measured v1 cycle overhead (%)")
    axes[0].set_ylim(0, 3.2)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, ncol=4, loc="upper center", bbox_to_anchor=(0.55, 1.02))
    style.panel_label(axes[0], "(a)")
    style.panel_label(axes[1], "(b)")
    fig.subplots_adjust(left=0.11, right=0.98, top=0.78, bottom=0.18, wspace=0.12)
    manifest: List[dict] = []
    style.save_figure(
        fig, "fig5r_e4_sustained_overhead", figures, manifest, [csv_path],
        {"timing": "v1 measured 4-burst", "fill_mov": "not plotted", "hbm3": "MISSING"},
        sample_count=len(rows),
        aggregation="mean of two runs, overhead versus NONE of the same workload and access mode",
        caption=(
            "Fig. 5r. Measured v1 swap overhead on repeated W2, GEMV, and DEC. "
            "Streaming and reuse are separate panels. The FILL/MOV cycle estimate is not drawn."
        ),
    )
    _merge(figures, manifest)


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    figures = root / "figures"
    results = root / "results" / "m2r"
    figure_4r(results / "E2_security.csv", figures)
    figure_4r_none(results / "E2_security.csv", figures)
    figure_3r(root / "results" / "m2r_g3_seeds" / "E3_multiseed.csv",
              root / "results" / "m2r_a3_horizon" / "A3_horizon.csv", figures)
    figure_3s(root / "results" / "m2r_swap_sensitivity" / "A1_swap_sensitivity.csv", figures)
    figure_5r(results / "E4_overhead.csv", figures)


if __name__ == "__main__":
    main()
