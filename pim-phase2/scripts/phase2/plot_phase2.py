#!/usr/bin/env python3
"""Render Phase 2 figures P2-F1..P2-F6 and the P2-T1 tables.

Only ``results/phase2/figure_data/*.csv`` is read.  The script never reruns the
simulator or the L2 evaluator.
"""
from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))

from p2style import (  # noqa: E402
    DOUBLE,
    POLICY_COLORS,
    POLICY_HATCH,
    POLICY_MARKER,
    POLICY_ORDER,
    SINGLE,
    STATUS_COLORS,
    DISTANCE_MARKERS,
    bootstrap_ci,
    number,
    panel_label,
    policy_style,
    read_csv,
    save_figure,
    wilson,
    write_manifest,
)

FIGDATA = ROOT / "results/phase2/figure_data"
FIGURES = ROOT / "figures/phase2"

CAPTIONS = {
    "p2f1_selectivity": (
        "P2-F1. Report granularity x policy selectivity for UE reports, n_tiles=32. "
        "(a) replayed tile operations as a percentage of all job tile operations; "
        "(b) recovery latency normalized to B0 for the same workload, granularity, "
        "delay and seed. X labels use bk=bank, pg=page, cw=codeword. Bars are means "
        "with 95% bootstrap CI; black short lines are the Oracle lower bound. W3 "
        "panel uses K=4; K=16 rows are in statistics.csv. ASSUMED: L2 costs for "
        "MUL/RELU."),
    "p2f2_breakdown": (
        "P2-F2. Recovery-latency breakdown for W1, page reports, n_tiles in {8,32}. "
        "Bars are means over 30 seeds x fault operands: quiesce, restore, replay, "
        "resume and publish. Diamonds are L3 measured totals for the E2E cases that "
        "exist (B0 UE, B1 UE, B2 CE at n=8); missing diamonds mean no L3 case was "
        "run for that exact combination."),
    "p2f3_release": (
        "P2-F3. No-fault steady-state overhead versus release delay Delta/T_job for "
        "GLOBAL_HOLD and DEP_RELEASE, W1 and W2. Lines are L2 means over 30 seeds "
        "with bootstrap CI shading; black x markers are the Phase 1 P1-F measured "
        "points for W1 (HOST consumer). Negative DEP_RELEASE values mean the host "
        "starts reading a tile before the kernel-level producer barrier."),
    "p2f4_radius": (
        "P2-F4. Missed-error coverage versus replay cost. Each curve is a silent "
        "distance class (CE report plus one latent error); markers are B2, B2R-0, "
        "B2R-1, B2R-2 and B0. Coverage uses Wilson 95% CI over 30 seeds; the "
        "distance distribution is ASSUMED, not measured. A minus-class case whose "
        "primary tile lies on the first row falls back to the corresponding plus "
        "class, so some minus and plus curves overlap."),
    "p2f5_status": (
        "P2-F5. P2Status distribution over 30 seeds for the status scenarios: normal "
        "UE, CE, Delta_cfg/Delta_true=0.5, Delta_cfg/Delta_true=1.0 with early "
        "consumption, missing checkpoint, truncated ledger and report-queue overflow."),
    "p2f6_validation": (
        "P2-F6. L2-predicted versus L3-measured recovery cycles after refitting "
        "the W1 per-tile cost on the L3 E2E-0 no-fault runs; W2 is the held-out "
        "validation set. Hollow points are the W1 fit set, filled points are W2. "
        "The dashed line is y=x. Both MAPE values are printed in the panel; the "
        "W2 validation MAPE is the out-of-sample number."),
}


def grouped(rows, keys):
    result = defaultdict(list)
    for row in rows:
        result[tuple(row[key] for key in keys)].append(row)
    return result


def mean_ci(values):
    clean = [value for value in values if value is not None]
    if not clean:
        return None, None, None
    low, high = bootstrap_ci(clean)
    return float(np.mean(clean)), low, high


def plot_f1(data: Path, manifest: list) -> None:
    path = data / "p2f1_selectivity.csv"
    rows = [row for row in read_csv(path) if row["workload"] in ("W1", "W2", "W3_K4")]
    workloads = ["W1", "W2", "W3_K4"]
    workload_labels = {"W1": "W1", "W2": "W2", "W3_K4": "W3"}
    granularities = ["bank", "page", "codeword"]
    policies = ["B0", "B1", "B2"]
    groups = grouped(rows, ["workload", "granularity", "policy"])
    oracle = grouped(rows, ["workload", "granularity", "policy"])

    fig, axes = plt.subplots(1, 2, figsize=DOUBLE)
    x_positions = {}
    index = 0
    centers = []
    labels = []
    short_gran = {"bank": "bk", "page": "pg", "codeword": "cw"}
    for workload in workloads:
        for granularity in granularities:
            x_positions[(workload, granularity)] = index
            centers.append(index)
            labels.append(f"{workload_labels[workload]}\n{short_gran[granularity]}")
            index += 1

    for axis, metric, ylabel, scale in (
        (axes[0], "replay_fraction", "Replayed tiles (% of job)", 100.0),
        (axes[1], "recovery_norm", "Recovery latency / B0", 1.0),
    ):
        width = 0.24
        for offset_index, policy in enumerate(policies):
            means, lows, highs, xs = [], [], [], []
            for workload in workloads:
                for granularity in granularities:
                    values = [number(row[metric]) for row in
                              groups.get((workload, granularity, policy), [])]
                    values = [value for value in values if value is not None]
                    mean, low, high = mean_ci(values)
                    if mean is None:
                        continue
                    xs.append(x_positions[(workload, granularity)]
                              + (offset_index - 1) * width)
                    means.append(mean * scale)
                    lows.append((mean - (low if low is not None else mean)) * scale)
                    highs.append(((high if high is not None else mean) - mean) * scale)
            axis.bar(xs, means, width=width, color=POLICY_COLORS[policy],
                     hatch=POLICY_HATCH[policy], edgecolor="white", linewidth=0.4,
                     yerr=[lows, highs], capsize=1.5, label=policy)
        oracle_x, oracle_y = [], []
        for workload in workloads:
            for granularity in granularities:
                values = [number(row[metric]) for row in
                          oracle.get((workload, granularity, "Oracle"), [])]
                values = [value for value in values if value is not None]
                mean, _low, _high = mean_ci(values)
                if mean is not None:
                    oracle_x.append(x_positions[(workload, granularity)])
                    oracle_y.append(mean * scale)
        axis.plot(oracle_x, oracle_y, linestyle="none", marker="_", markersize=13,
                  markeredgewidth=2.0, color="black", label="Oracle")
        axis.set_xticks(centers)
        axis.set_xticklabels(labels, fontsize=7.5)
        axis.set_ylabel(ylabel)
        axis.set_ylim(bottom=0)
        for tick in axis.get_xticklabels():
            tick.set_linespacing(0.9)
    panel_label(axes[0], "(a)")
    panel_label(axes[1], "(b)")
    handles, labels_ = axes[0].get_legend_handles_labels()
    order = [labels_.index(name) for name in ("B0", "B1", "B2", "Oracle")
             if name in labels_]
    fig.legend([handles[i] for i in order], [labels_[i] for i in order],
               loc="upper center", ncol=4, frameon=False,
               bbox_to_anchor=(0.5, 1.12))
    save_figure(fig, "p2f1_selectivity", FIGURES, manifest, [path],
                {"error_class": "UE", "n_tiles": 32,
                 "workloads": ["W1", "W2", "W3_K4"]},
                len(rows), "mean per workload/granularity/policy; 95% bootstrap CI",
                CAPTIONS)


def plot_f2(data: Path, manifest: list) -> None:
    path = data / "p2f2_breakdown.csv"
    rows = [row for row in read_csv(path)
            if row["workload"] == "W1" and row["granularity"] == "page"]
    components = [("quiesce_cycles", "quiesce", "#0072B2"),
                  ("restore_cycles", "restore", "#009E73"),
                  ("replay_cycles", "replay", "#D55E00"),
                  ("resume_cycles", "resume", "#CC79A7"),
                  ("publish_cycles", "publish", "#E69F00")]
    categories = []
    for n_tiles in (8, 32):
        for error_class in ("CE", "UE"):
            for policy in ("B0", "B1", "B2"):
                categories.append((n_tiles, error_class, policy))
    groups = grouped(rows, ["n_tiles", "error_class", "policy"])
    fig, axis = plt.subplots(figsize=SINGLE)
    xs = np.arange(len(categories))
    bottom = np.zeros(len(categories))
    for key, label, color in components:
        values = []
        for category in categories:
            group = groups.get(tuple(str(v) for v in category), [])
            numbers = [number(row[key]) for row in group]
            numbers = [value for value in numbers if value is not None]
            values.append(float(np.mean(numbers)) if numbers else 0.0)
        axis.bar(xs, values, bottom=bottom, color=color, label=label,
                 edgecolor="white", linewidth=0.4, width=0.72)
        bottom += np.array(values)
    l3_path = data / "p2f2_l3.csv"
    if l3_path.exists() and read_csv(l3_path):
        l3 = read_csv(l3_path)
        l3_groups = grouped(l3, ["n_tiles", "error_class", "policy"])
        for index, category in enumerate(categories):
            key = tuple(str(v) for v in category)
            values = [number(row["recovery_cycles"]) for row in l3_groups.get(key, [])]
            values = [value for value in values if value is not None]
            if values:
                axis.plot(index, float(np.mean(values)), marker="D", linestyle="none",
                          color="black", markersize=4)
    axis.set_xticks(xs)
    axis.set_xticklabels([f"{n}\n{ec}\n{policy}" for n, ec, policy in categories],
                         fontsize=6.8)
    axis.set_ylabel("Recovery latency (DRAM cycles)")
    axis.set_ylim(bottom=0)
    axis.legend(loc="upper center", bbox_to_anchor=(0.5, 1.18), ncol=5,
                frameon=False)
    save_figure(fig, "p2f2_breakdown", FIGURES, manifest,
                [path] + ([l3_path] if l3_path.exists() else []),
                {"workload": "W1", "granularity": "page", "n_tiles": [8, 32]},
                len(rows), "stacked means over 30 seeds x fault operands", CAPTIONS)


def plot_f3(data: Path, manifest: list) -> None:
    path = data / "p2f3_release.csv"
    rows = [row for row in read_csv(path) if row["n_tiles"] == "8"]
    l2 = [row for row in rows if row["source"] == "L2"]
    p1 = [row for row in rows if row["source"] == "P1-F"]
    fig, axis = plt.subplots(figsize=SINGLE)
    colors = {"W1": "#0072B2", "W2": "#D55E00"}
    linestyles = {"GLOBAL_HOLD": "-", "DEP_RELEASE": "--"}
    for workload in ("W1", "W2"):
        for release in ("GLOBAL_HOLD", "DEP_RELEASE"):
            groups = grouped([row for row in l2
                              if row["workload"] == workload and row["release"] == release],
                             ["delta_over_job"])
            xs, means, lows, highs = [], [], [], []
            for (ratio,), group in sorted(groups.items(), key=lambda item: float(item[0][0])):
                values = [number(row["overhead_fraction"]) * 100.0 for row in group]
                mean, low, high = mean_ci(values)
                if mean is None:
                    continue
                xs.append(float(ratio))
                means.append(mean)
                lows.append((low if low is not None else mean))
                highs.append((high if high is not None else mean))
            axis.plot(xs, means, marker="o", markersize=3.5,
                      color=colors[workload], linestyle=linestyles[release],
                      label=f"{workload} {release}")
            axis.fill_between(xs, lows, highs, color=colors[workload], alpha=0.12,
                              linewidth=0)
    if p1:
        px, py = [], []
        for row in p1:
            ratio = number(row["delta_over_job"])
            overhead = number(row["overhead_fraction"])
            if ratio is not None and overhead is not None:
                px.append(ratio)
                py.append(overhead * 100.0)
        axis.plot(px, py, linestyle="none", marker="x", color="black",
                  markersize=5, label="P1-F measured (W1, HOST)")
    axis.axhline(0.0, color="#999999", linewidth=0.6)
    axis.set_xlabel(r"Release delay $\Delta/T_{\mathrm{job}}$")
    axis.set_ylabel("Steady-state overhead (%)")
    axis.legend(loc="upper center", bbox_to_anchor=(0.5, 1.22), ncol=3,
                frameon=False, fontsize=7)
    save_figure(fig, "p2f3_release", FIGURES, manifest, [path],
                {"workloads": ["W1", "W2"], "fault": "none"},
                len(rows), "mean per workload/release/ratio; 95% bootstrap CI", CAPTIONS)


def plot_f4(data: Path, manifest: list) -> None:
    path = data / "p2f4_radius.csv"
    rows = read_csv(path)
    distances = ["same_row", "minus1", "plus1", "minus2", "plus2", "far"]
    policies = ["B2", "B2R-0", "B2R-1", "B2R-2", "B0"]
    groups = grouped(rows, ["distance_class", "policy"])
    fig, axis = plt.subplots(figsize=SINGLE)
    cmap = plt.get_cmap("tab10")
    for index, distance in enumerate(distances):
        xs, ys, low_err, high_err, markers = [], [], [], [], []
        for policy in policies:
            group = groups.get((distance, policy), [])
            if not group:
                continue
            x = float(np.mean([number(row["replay_fraction"]) * 100.0 for row in group]))
            k = sum(int(float(row["silent_repaired"])) for row in group)
            p, low, high = wilson(k, len(group))
            if p is None:
                continue
            xs.append(x)
            ys.append(p * 100.0)
            low_err.append((p - low) * 100.0)
            high_err.append((high - p) * 100.0)
            markers.append(policy)
        if not xs:
            continue
        axis.errorbar(xs, ys, yerr=[low_err, high_err], color=cmap(index),
                      linewidth=1.0, capsize=1.5, label=distance)
        for policy, x, y in zip(markers, xs, ys):
            axis.plot(x, y, marker=POLICY_MARKER[policy], color=cmap(index),
                      markersize=5.0 if policy != "B0" else 6.0,
                      markerfacecolor="none" if policy in ("B2", "B2R-0") else cmap(index),
                      markeredgewidth=1.0)
    axis.set_xlabel("Replayed tiles (% of job)")
    axis.set_ylabel("Silent-error coverage (%)")
    axis.set_ylim(-3, 103)
    distance_handles = [plt.Line2D([], [], color=cmap(i), linewidth=1.2,
                                   label=distances[i]) for i in range(len(distances))]
    policy_handles = [plt.Line2D([], [], color="black", linestyle="none",
                                 marker=POLICY_MARKER[policy], markersize=4.5,
                                 label=policy) for policy in policies]
    axis.legend(handles=distance_handles, loc="lower right", frameon=False,
                fontsize=6.3, title="silent distance", title_fontsize=6.5)
    fig.legend(handles=policy_handles, loc="upper center", frameon=False,
               ncol=5, fontsize=6.5, title="policy", title_fontsize=6.5,
               bbox_to_anchor=(0.5, 1.16))
    save_figure(fig, "p2f4_radius", FIGURES, manifest, [path],
                {"error_class": "CE", "n_tiles": 32,
                 "distance_classes": distances, "policies": policies},
                len(rows), "Wilson 95% coverage and mean replay fraction", CAPTIONS)


def plot_f5(data: Path, manifest: list) -> None:
    path = data / "p2f5_status.csv"
    rows = read_csv(path)
    scenarios = ["NORMAL_UE", "CE", "DELTA_SHORT_0.5", "DELTA_FULL_1.0",
                 "CHECKPOINT_MISSING", "LEDGER_TRUNCATED", "REPORT_OVERFLOW"]
    statuses = ["SOURCE_REPAIRED_ONLY", "SELECTIVE_RECOVERED", "PARTIAL_WITH_ESCAPE",
                "WHOLE_JOB_FALLBACK", "OUTSIDE_WINDOW_NO_TRUSTED_INPUT"]
    groups = grouped(rows, ["scenario", "status"])
    totals = defaultdict(int)
    for row in rows:
        totals[row["scenario"]] += 1
    fig, axis = plt.subplots(figsize=SINGLE)
    left = np.zeros(len(scenarios))
    for status in statuses:
        values = np.array([100.0 * len(groups.get((scenario, status), []))
                           / max(1, totals[scenario]) for scenario in scenarios])
        axis.barh(np.arange(len(scenarios)), values, left=left,
                  color=STATUS_COLORS[status], label=status, height=0.65,
                  edgecolor="white", linewidth=0.4)
        left += values
    axis.set_yticks(np.arange(len(scenarios)))
    axis.set_yticklabels(scenarios, fontsize=7)
    axis.invert_yaxis()
    axis.set_xlabel("Share of runs (%)")
    axis.set_xlim(0, 100)
    axis.legend(loc="upper center", bbox_to_anchor=(0.5, 1.20), ncol=2,
                frameon=False, fontsize=6.5)
    save_figure(fig, "p2f5_status", FIGURES, manifest, [path],
                {"seeds": 30, "workload": "W1", "n_tiles": 32},
                len(rows), "share of P2Status per scenario", CAPTIONS)


def plot_f6(data: Path, manifest: list) -> None:
    path = data / "p2f6_validation.csv"
    if not path.exists():
        return
    rows = read_csv(path)
    if not rows:
        return
    fig, axis = plt.subplots(figsize=SINGLE)
    for workload, filled in (("W1", False), ("W2", True)):
        subset = [row for row in rows if row["workload"] == workload]
        if not subset:
            continue
        xs = [number(row["l3_recovery_cycles"]) for row in subset]
        ys = [number(row["l2_recovery_cycles"]) for row in subset]
        axis.scatter(xs, ys, s=22, facecolors="black" if filled else "none",
                     edgecolors="#0072B2" if filled else "black", linewidths=0.8,
                     label=workload + (" validation" if filled else " fit"),
                     marker="o" if filled else "o")
    finite = [(number(row["l3_recovery_cycles"]), number(row["l2_recovery_cycles"]),
               row["split"]) for row in rows]
    finite = [(x, y, split) for x, y, split in finite if x and y]
    if finite:
        low = min(min(x, y) for x, y, _split in finite) * 0.9
        high = max(max(x, y) for x, y, _split in finite) * 1.05
        axis.plot([low, high], [low, high], linestyle="--", color="#666666",
                  linewidth=0.8)
        def mape_of(split_name):
            values = [(x, y) for x, y, split in finite if split == split_name]
            if not values:
                return None
            return float(np.mean([abs(x - y) / x for x, y in values])) * 100.0
        fit_mape = mape_of("fit")
        validation_mape = mape_of("validation")
        lines = []
        if fit_mape is not None:
            lines.append(f"W1 fit MAPE = {fit_mape:.1f}%")
        if validation_mape is not None:
            lines.append(f"W2 validation MAPE = {validation_mape:.1f}%")
        axis.text(0.03, 0.95, "\n".join(lines), transform=axis.transAxes,
                  va="top", fontsize=7.5)
    axis.set_xlabel("L3 measured recovery cycles")
    axis.set_ylabel("L2 predicted recovery cycles")
    axis.legend(frameon=False, loc="lower right")
    save_figure(fig, "p2f6_validation", FIGURES, manifest, [path],
                {"workloads": ["W1", "W2"]}, len(rows),
                "per run: L2 prediction vs L3 measurement", CAPTIONS)


def write_t1_tables(data: Path) -> None:
    path = data / "p2t1_ledger.csv"
    rows = read_csv(path)
    order = {"W1": 0, "W2": 1, "W3_K4": 2, "W3_K16": 3}
    rows.sort(key=lambda row: (order.get(row["workload"], 9), int(row["n_tiles"]),
                               row["release"], float(row["delta_over_job"])))
    markdown = ["| Workload | n_tiles | Release | Delta/T | Entries | Est. bytes | Peak held |",
                "|---|---:|---|---:|---:|---:|---:|"]
    for row in rows:
        markdown.append(
            f"| {row['workload']} | {row['n_tiles']} | {row['release']} | "
            f"{float(row['delta_over_job']):.2f} | {row['entries']} | "
            f"{row['bytes_est']} | {row['peak_held_items']} |")
    (FIGURES / "p2t1_ledger.md").write_text("\n".join(markdown) + "\n")
    latex = [
        r"\begin{table}[t]",
        r"\centering",
        r"\begin{tabular}{lrrrrrr}",
        r"\toprule",
        r"Workload & $n$ & Release & $\Delta/T$ & Entries & Bytes (est.) & Peak held \\",
        r"\midrule",
    ]
    for row in rows:
        workload = str(row["workload"]).replace("_", r"\_")
        release = str(row["release"]).replace("_", r"\_")
        ratio = float(row["delta_over_job"])
        latex.append(
            f"{workload} & {row['n_tiles']} & {release} & "
            f"{ratio:.2f} & {row['entries']} & "
            f"{row['bytes_est']} & {row['peak_held_items']} " + r"\\")
    latex += [r"\bottomrule", r"\end{tabular}",
              r"\caption{Phase 2 ledger overhead (P2-T1).}", r"\end{table}"]
    (FIGURES / "p2t1_ledger.tex").write_text("\n".join(latex) + "\n")


def write_captions(manifest: list, sample_counts: dict) -> None:
    lines = ["# Phase 2 figure captions", ""]
    for name, caption in CAPTIONS.items():
        lines.append(f"## {name}")
        lines.append("")
        lines.append(caption)
        lines.append("")
        if name in sample_counts:
            lines.append(f"- Sample rows: {sample_counts[name]}")
        lines.append("- Source: `results/phase2/figure_data/` (L2 unless a P1-F or L3 "
                     "source column is present).")
        lines.append("- ASSUMED: MUL/RELU per-tile cost until L3 correction; silent "
                     "distance distribution; single channel; host copy/OS overhead "
                     "not modelled.")
        lines.append("")
    lines.append("## p2t1_ledger")
    lines.append("")
    lines.append("P2-T1. Ledger overhead per job: entries, estimated bytes and peak "
                 "held data items under GLOBAL_HOLD and DEP_RELEASE. Values are "
                 "produced by the L2 ledger model (`source=L2`).")
    lines.append("")
    (FIGURES / "captions.md").write_text("\n".join(lines))


def main() -> int:
    global FIGDATA, FIGURES
    parser = argparse.ArgumentParser()
    parser.add_argument("--figure-data", default=str(FIGDATA))
    parser.add_argument("--figures", default=str(FIGURES))
    args = parser.parse_args()
    FIGDATA = Path(args.figure_data)
    FIGURES = Path(args.figures)
    FIGURES.mkdir(parents=True, exist_ok=True)
    manifest: list = []
    plot_f1(FIGDATA, manifest)
    plot_f2(FIGDATA, manifest)
    plot_f3(FIGDATA, manifest)
    plot_f4(FIGDATA, manifest)
    plot_f5(FIGDATA, manifest)
    plot_f6(FIGDATA, manifest)
    write_t1_tables(FIGDATA)
    counts = {name: len(read_csv(FIGDATA / f"{name}.csv"))
              for name in CAPTIONS if (FIGDATA / f"{name}.csv").exists()}
    write_captions(manifest, counts)
    write_manifest(FIGURES / "plot_manifest.json", manifest)
    print(f"FIGURES={len(manifest)} TABLES=2")
    for entry in manifest:
        print(entry["figure"], entry["sample_count"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
