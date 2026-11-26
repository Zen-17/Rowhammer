#!/usr/bin/env python3
"""Figure 6, figure 7, table 2, and the E6 ablation chart.

The 14.5K runs stay in their own table block.  They are not drawn on figure 6.
"""
from __future__ import annotations

import csv
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

import matplotlib.pyplot as plt
import numpy as np

from phase3_exp.plot.style import (
    ABLATION_HATCH, DOUBLE, SCHEME_COLORS, SCHEME_ORDER, panel_label, save_figure,
    scheme_color, write_manifest,
)

EXP = ROOT / "phase3_exp"
RESULTS = EXP / "results/m3"
FIGURES = EXP / "figures"
ATTACKS = ["A1", "A3", "A4"]
WORKLOADS = ["W2", "DEC"]
LETTERS = list("abcdef")
ABLATION_ORDER = ["OURS", "OURS-CE", "OURS-SCRUB", "OURS-EPOCH", "OURS-G1.5", "OURS-DET"]
ABLATION_LABELS = ["OURS", "−CE", "−SCRUB", "−EPOCH", "G1.5", "−DET"]


def read_csv(path: Path) -> list:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def num(row: dict, key: str) -> float:
    text = row.get(key, "")
    if text is None or text == "":
        return 0.0
    return float(text)


def cell_map(rows: list) -> dict:
    return {(row["workload"], row["scheme"], row["attack"]): row for row in rows}


def figure_6(summary: list, manifest: list) -> None:
    cells = cell_map(summary)
    fig, axes = plt.subplots(2, 3, figsize=DOUBLE, sharex=False, sharey=True)
    for row_index, workload in enumerate(WORKLOADS):
        for col_index, attack in enumerate(ATTACKS):
            axis = axes[row_index][col_index]
            panel_label(axis, f"({LETTERS[row_index * 3 + col_index]})")
            if row_index == 0:
                axis.set_title(attack)
            baseline = cells.get((workload, "NONE", attack))
            base_cycles = num(baseline, "mean_cycles") if baseline else 1.0
            if base_cycles <= 0:
                base_cycles = 1.0
            for scheme in SCHEME_ORDER:
                cell = cells.get((workload, scheme, attack))
                if cell is None or cell.get("device") == "HBM3":
                    continue
                overhead = 100.0 * (num(cell, "mean_cycles") - base_cycles) / base_cycles
                rate = num(cell, "o6_rate")
                low = num(cell, "wilson_low")
                high = num(cell, "wilson_high")
                axis.errorbar(
                    overhead, rate, yerr=[[max(0.0, rate - low)], [max(0.0, high - rate)]],
                    fmt="o", color=SCHEME_COLORS[scheme], markersize=3.4, elinewidth=0.7,
                    capsize=1.6, label=scheme,
                )
                if int(float(cell["O6"])) == 0 and cell.get("zero_escape_upper"):
                    slot = SCHEME_ORDER.index(scheme)
                    axis.annotate(
                        "3/N", xy=(overhead, rate),
                        xytext=((slot - 2.5) * 16, 3 + slot * 5),
                        textcoords="offset points", ha="center", va="bottom", fontsize=5.5,
                        color=SCHEME_COLORS[scheme],
                    )
            if col_index == 0:
                axis.set_ylabel("O6 fraction")
            if row_index == 1:
                axis.set_xlabel("Cycle overhead vs NONE (%)")
            axis.set_ylim(-0.02, 1.05)
    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=6, frameon=False,
               bbox_to_anchor=(0.5, 1.02))
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    save_figure(
        fig, "fig6_overhead_vs_failure", FIGURES, manifest,
        [RESULTS / "E5_campaign_summary.csv"],
        {"scale": "1/16", "device": "HBM2", "excluded": "14.5K and HBM3"},
        4800, "mean cycles and Wilson 95% on the O6 fraction",
        "Figure 6. O6 fraction against cycle overhead relative to NONE. "
        "Columns are attacks and rows are workloads. Whiskers are Wilson 95% intervals. "
        "A point with zero escapes is labeled with the 3/N bound. The 14.5K runs are not on this figure.",
    )


def figure_7(summary: list, manifest: list) -> None:
    cells = cell_map(summary)
    reference = json.loads((RESULTS / "reference_throughput.json").read_text())
    fig, axes = plt.subplots(2, 3, figsize=DOUBLE, sharey=True)
    for row_index, workload in enumerate(WORKLOADS):
        denom = float(reference.get(f"{workload}|0.0625", 0.0) or 0.0)
        if denom <= 0:
            denom = 1.0
        for col_index, attack in enumerate(ATTACKS):
            axis = axes[row_index][col_index]
            panel_label(axis, f"({LETTERS[row_index * 3 + col_index]})")
            if row_index == 0:
                axis.set_title(attack)
            heights = []
            colors = []
            for scheme in SCHEME_ORDER:
                cell = cells[(workload, scheme, attack)]
                heights.append(num(cell, "mean_effective_throughput") / denom)
                colors.append(SCHEME_COLORS[scheme])
            axis.bar(np.arange(len(SCHEME_ORDER)), heights, color=colors, width=0.72)
            axis.set_xticks(np.arange(len(SCHEME_ORDER)))
            axis.set_xticklabels(SCHEME_ORDER, rotation=45, ha="right")
            if col_index == 0:
                axis.set_ylabel("Throughput")
            axis.set_ylim(0, max(1.05, max(heights) * 1.25 if heights else 1.05))
    handles = [plt.Rectangle((0, 0), 1, 1, color=SCHEME_COLORS[name]) for name in SCHEME_ORDER]
    fig.legend(handles, SCHEME_ORDER, loc="upper center", ncol=6, frameon=False,
               bbox_to_anchor=(0.5, 1.02))
    fig.tight_layout(rect=(0, 0, 1, 0.90))
    save_figure(
        fig, "fig7_e5_throughput", FIGURES, manifest,
        [RESULTS / "E5_campaign_summary.csv", RESULTS / "reference_throughput.json"],
        {"scale": "1/16", "o5_o6": "counted as zero throughput"},
        4800, "mean effective throughput, O5 and O6 included as zero",
        "Figure 7. Released throughput divided by a fault-free NONE run of the same workload. "
        "O5 and O6 contribute zero and stay in the mean. The 14.5K runs are not on this figure.",
    )


def table_2(summary: list, real_summary: list) -> None:
    fields = [
        "block", "workload", "scheme", "attack", "n", "O1", "O2", "O3", "O4", "O5", "O6",
        "o6_wilson", "zero_escape_upper", "mean_latent_flips", "effective_throughput",
    ]
    rows = []
    for block, source in (("1/16", summary), ("14.5K", real_summary)):
        for cell in source:
            rows.append({
                "block": block, "workload": cell["workload"], "scheme": cell["scheme"],
                "attack": cell["attack"], "n": cell["n_success"],
                "O1": cell["O1"], "O2": cell["O2"], "O3": cell["O3"],
                "O4": cell["O4"], "O5": cell["O5"], "O6": cell["O6"],
                "o6_wilson": f"[{cell['wilson_low']}, {cell['wilson_high']}]",
                "zero_escape_upper": cell["zero_escape_upper"],
                "mean_latent_flips": cell["mean_latent_flips"],
                "effective_throughput": cell["mean_effective_throughput"],
            })
    path = RESULTS / "table2_e5_campaign.csv"
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    (EXP / "results" / "table2_e5_campaign.csv").write_text(path.read_text())
    lines = [
        "\\begin{tabular}{ll ll r rrrrrr l l r r}",
        "\\hline",
        "block & WL & atk & scheme & N & O1 & O2 & O3 & O4 & O5 & O6 & Wilson & 3/N & latent & thr \\\\",
        "\\hline",
    ]
    for row in rows:
        lines.append(
            f"{row['block']} & {row['workload']} & {row['attack']} & {row['scheme']} & {row['n']} & "
            f"{row['O1']} & {row['O2']} & {row['O3']} & {row['O4']} & {row['O5']} & {row['O6']} & "
            f"{row['o6_wilson']} & {row['zero_escape_upper']} & {row['mean_latent_flips']} & "
            f"{row['effective_throughput']} \\\\"
        )
    lines.append("\\hline\n\\end{tabular}\n")
    text = "\n".join(lines)
    (RESULTS / "table2_e5_campaign.tex").write_text(text)
    (EXP / "results" / "table2_e5_campaign.tex").write_text(text)


def figure_e6(e5: list, e6: list, manifest: list) -> None:
    ours = cell_map(e5)
    abl = cell_map(e6)
    fig, axes = plt.subplots(1, 2, figsize=DOUBLE, sharey=True)
    for index, attack in enumerate(("A1", "A3")):
        axis = axes[index]
        panel_label(axis, f"({LETTERS[index]})")
        axis.set_title(attack)
        full = ours[("DEC", "OURS", attack)]
        full_cycles = num(full, "mean_cycles") or 1.0
        o6_vals = []
        mix_vals = []
        deltas = []
        for name in ABLATION_ORDER:
            cell = full if name == "OURS" else abl[("DEC", name, attack)]
            n = float(cell["n_success"]) or 1.0
            o6_vals.append(float(cell["O6"]) / n)
            mix_vals.append((float(cell["O4"]) + float(cell["O5"])) / n)
            deltas.append(100.0 * (num(cell, "mean_cycles") - full_cycles) / full_cycles)
        positions = np.arange(len(ABLATION_ORDER))
        for offset, values, color, label in (
            (-0.18, o6_vals, scheme_color("OURS"), "O6"),
            (0.18, mix_vals, scheme_color("FIX"), "O4+O5"),
        ):
            bars = axis.bar(positions + offset, values, width=0.34, color=color, label=label)
            for bar, name in zip(bars, ABLATION_ORDER):
                if name != "OURS":
                    bar.set_hatch(ABLATION_HATCH)
        top = max(o6_vals + mix_vals + [0.05])
        for position, delta, left, right in zip(positions, deltas, o6_vals, mix_vals):
            axis.text(position, max(left, right) + top * 0.04, f"{delta:+.1f}%",
                      ha="center", va="bottom", fontsize=5.5)
        axis.set_xticks(positions)
        axis.set_xticklabels(ABLATION_LABELS, rotation=30, ha="right")
        axis.set_ylim(0, top * 1.35 + 0.02)
        if index == 0:
            axis.set_ylabel("Fraction of runs")
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles[:2], labels[:2], loc="upper center", ncol=2, frameon=False,
               bbox_to_anchor=(0.5, 1.08))
    fig.tight_layout()
    save_figure(
        fig, "fig_e6_ablation", FIGURES, manifest,
        [RESULTS / "E6_ablation.csv", RESULTS / "E5_campaign_summary.csv"],
        {"workload": "DEC", "attacks": "A1,A3", "scale": "1/16"},
        1000, "O6 and O4+O5 fractions; overhead delta annotated against full OURS",
        "Figure E6. Full OURS against the five ablations on DEC. Hatched bars are ablations. "
        "The annotation is the cycle-overhead difference from full OURS, not a second axis.",
    )


def main() -> None:
    summary = read_csv(RESULTS / "E5_campaign_summary.csv")
    real_summary = read_csv(RESULTS / "E5_real_summary.csv") if (RESULTS / "E5_real_summary.csv").exists() else []
    e6 = read_csv(RESULTS / "E6_ablation.csv") if (RESULTS / "E6_ablation.csv").exists() else []
    manifest_path = FIGURES / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else []
    fresh = {"fig6_overhead_vs_failure", "fig7_e5_throughput", "fig_e6_ablation"}
    manifest = [entry for entry in manifest if entry.get("figure") not in fresh]
    figure_6(summary, manifest)
    figure_7(summary, manifest)
    table_2(summary, real_summary)
    if e6:
        figure_e6(summary, e6, manifest)
    write_manifest(FIGURES, manifest)
    print("figures written", flush=True)


if __name__ == "__main__":
    main()
