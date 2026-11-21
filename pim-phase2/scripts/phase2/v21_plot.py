#!/usr/bin/env python3
"""Render the v2.1 figures P2-F1..P2-F7 and tables P2-T1/P2-T2."""
from __future__ import annotations

import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))

from v21_style import (  # noqa: E402
    COLORS,
    DOUBLE,
    MARKERS,
    SINGLE,
    STATUS_COLORS,
    WIDE,
    panel_label,
    save_figure,
)

FIGDATA = ROOT / "results/phase2/v21/figure_data"
FIGURES = ROOT / "figures/phase2/v21"
RUNS = ROOT / "runs/phase2/v21/l3"


def read_csv(path: Path) -> list[dict]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def num(value) -> float | None:
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def mean(values):
    clean = [value for value in values if value is not None]
    return statistics.mean(clean) if clean else None


def plot_f1(manifest: list) -> None:
    path = FIGDATA / "p2f1_selectivity.csv"
    rows = [row for row in read_csv(path)
            if row["workload"] in ("W1", "W2", "W3_K4")
            and row["granularity"] in ("bank", "page")]
    workloads = ["W1", "W2", "W3_K4"]
    granularities = ["bank", "page"]
    policies = ["B1", "B2"]
    delays = ["16", "256", "4096"]
    groups = defaultdict(list)
    for row in rows:
        groups[(row["workload"], row["granularity"], row["policy"],
                row["transport_delay"])].append(row)
    fig, axes = plt.subplots(2, 3, figsize=(7.16, 4.15), sharey="row")
    x_positions = {}
    centers = []
    labels = []
    index = 0
    short_workload = {"W1": "W1", "W2": "W2", "W3_K4": "W3"}
    for workload in workloads:
        for granularity in granularities:
            x_positions[(workload, granularity)] = index
            centers.append(index)
            labels.append(f"{short_workload[workload]}\n{'bk' if granularity == 'bank' else 'pg'}")
            index += 1
    width = 0.32
    row_max = {0: 0.0, 1: 0.0}
    for col, delay in enumerate(delays):
        for row_idx, metric, ylabel, scale in (
                (0, "affected_fraction", "Affected tile operations (%)", 100.0),
                (1, "fault_overhead_norm", "Fault overhead / B0", 1.0)):
            axis = axes[row_idx][col]
            for offset_idx, policy in enumerate(policies):
                means, xs = [], []
                for workload in workloads:
                    for granularity in granularities:
                        group = groups.get((workload, granularity, policy, delay), [])
                        value = mean(num(row[metric]) for row in group)
                        if value is None:
                            continue
                        xs.append(x_positions[(workload, granularity)]
                                  + (offset_idx - 0.5) * width)
                        means.append(value * scale)
                        row_max[row_idx] = max(row_max[row_idx], value * scale)
                axis.bar(xs, means, width=width, color=COLORS[policy],
                         edgecolor="white", linewidth=0.35, label=policy)
            oracle_x, oracle_y = [], []
            for workload in workloads:
                for granularity in granularities:
                    group = groups.get((workload, granularity, "Oracle", delay), [])
                    value = mean(num(row[metric]) for row in group)
                    if value is not None:
                        oracle_x.append(x_positions[(workload, granularity)])
                        oracle_y.append(value * scale)
            axis.plot(oracle_x, oracle_y, linestyle="none", marker="_",
                      markersize=11, markeredgewidth=1.6, color="black",
                      label="Oracle")
            axis.set_xticks(centers)
            if row_idx == 1:
                axis.set_xticklabels(labels, fontsize=6)
            else:
                axis.set_xticklabels([])
            axis.set_ylabel(ylabel if col == 0 else "")
            axis.set_ylim(bottom=0)
            if row_idx == 1:
                axis.axhline(1.0, color="#999999", linewidth=0.5, linestyle=":")
            axis.text(0.98, 0.92, f"D={delay}", transform=axis.transAxes,
                      ha="right", va="top", fontsize=6.5)
    for row_idx in range(2):
        for col in range(3):
            axes[row_idx][col].set_ylim(0, row_max[row_idx] * 1.08)
            panel_label(axes[row_idx][col], f"({chr(97 + row_idx * 3 + col)})")
    handles, labels_ = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels_, loc="upper center", ncol=4, frameon=False,
               bbox_to_anchor=(0.5, 1.04))
    save_figure(
        fig, "p2f1_selectivity", FIGURES, manifest, [path],
        {"error_class": "UE", "n_tiles": 32, "delays": [16, 256, 4096]},
        len(rows),
        "mean per workload/granularity/policy/D; B0-normalized fault overhead",
        "P2-F1. Report granularity x policy for UE reports, n_tiles=32. "
        "(a-c) affected tile operations as a fraction of all job tile operations; "
        "(d-f) fault_overhead_cycles normalized to B0 for the same key. Columns "
        "are transport delays D={16,256,4096}. B1/B2 are drawn; B0 is the "
        "whole-job baseline (100% replay) and is the 1.0 line in the lower row. "
        "Bank reports use the v2.1 R_read filter and replay coalescing. Oracle is "
        "the lower bound. ASSUMED: L2 MUL/RELU costs from the v2.1 L3 no-fault fit.")


def plot_f2(manifest: list) -> None:
    path = FIGDATA / "p2f2_breakdown.csv"
    rows = read_csv(path)
    groups = defaultdict(list)
    for row in rows:
        if row["source"] == "L2":
            groups[(int(row["n_tiles"]), row["error_class"], row["policy"])].append(row)
    l3 = defaultdict(list)
    for row in rows:
        if row["source"] == "L3":
            l3[(int(row["n_tiles"]), row["error_class"], row["policy"])].append(
                num(row["recovery_cycles_excl_resume"]))
    fig, axes = plt.subplots(2, 2, figsize=(7.16, 3.3), sharex="col",
                             gridspec_kw={"height_ratios": [1.0, 2.4]})
    components = [("quiesce_cycles", "quiesce", "#0072B2"),
                  ("restore_cycles", "restore", "#009E73"),
                  ("replay_cycles", "replay", "#E69F00"),
                  ("publish_cycles", "publish", "#CC79A7")]
    for col, n_tiles in enumerate((8, 32)):
        top = axes[0][col]
        bottom = axes[1][col]
        categories = [(n_tiles, "CE", "B1"), (n_tiles, "CE", "B2"),
                      (n_tiles, "UE", "B1"), (n_tiles, "UE", "B2")]
        xs = np.arange(len(categories))
        resume = [mean(num(row["resume_cycles"]) for row in groups.get(category, []))
                  or 0.0 for category in categories]
        top.bar(xs, resume, width=0.52, color="#F4D7E8", edgecolor="#8E3B66",
                linewidth=0.6, hatch="////", label="resume (not in total)")
        top.set_ylim(0, max(resume) * 1.15 if max(resume) else 1)
        top.set_ylabel("resume\n(cycles)" if col == 0 else "")
        top.tick_params(axis="x", labelbottom=False)
        bottom_height = np.zeros(len(categories))
        for key, label, color in components:
            values = [mean(num(row[key]) for row in groups.get(category, [])) or 0.0
                      for category in categories]
            bottom.bar(xs, values, bottom=bottom_height, width=0.52, color=color,
                       edgecolor="white", linewidth=0.35, label=label)
            bottom_height += np.array(values)
        bottom.set_ylim(0, max(bottom_height) * 1.22 if max(bottom_height) else 1)
        bottom.set_ylabel("total excl. resume\n(cycles)" if col == 0 else "")
        for index, category in enumerate(categories):
            values = [value for value in l3.get(category, []) if value is not None]
            if values:
                bottom.plot(index, statistics.mean(values), marker="D",
                            linestyle="none", color="black", markersize=3.6,
                            label="L3 measured" if index == 0 and n_tiles == 8 else None)
        b0 = mean(num(row["recovery_cycles_excl_resume"])
                  for row in groups.get((n_tiles, "CE", "B0"), []))
        if b0:
            bottom.text(0.02, 0.96, f"B0 total excl. resume $\\approx$ {b0:,.0f}",
                        transform=bottom.transAxes, va="top", fontsize=6)
        bottom.set_xticks(xs)
        bottom.set_xticklabels(["CE\nB1", "CE\nB2", "UE\nB1", "UE\nB2"], fontsize=6.5)
        panel_label(top, "(a)" if n_tiles == 8 else "(b)")
    handles, labels_ = axes[0][0].get_legend_handles_labels()
    handles2, labels2 = axes[1][0].get_legend_handles_labels()
    fig.legend(handles + handles2, labels_ + labels2, loc="upper center", ncol=6,
               frameon=False, bbox_to_anchor=(0.5, 1.10), fontsize=6.0)
    save_figure(
        fig, "p2f2_breakdown", FIGURES, manifest, [path],
        {"workload": "W1", "granularity": "page", "n_tiles": [8, 32]},
        len(rows), "broken axis: resume separately above total excl. resume",
        "P2-F2. W1 page recovery breakdown. (a) n_tiles=8; (b) n_tiles=32. The "
        "upper strip is resume (hatched); the lower panel is the total excluding "
        "resume, stacked as quiesce+restore+replay+publish. Resume is separated "
        "because it is work the no-fault job would also do. B0 totals are "
        "annotated. Black diamonds are L3 measured recovery_cycles_excl_resume.")


def plot_f3(manifest: list) -> None:
    path = FIGDATA / "p2f3_release.csv"
    rows = [row for row in read_csv(path) if row["n_tiles"] == "8"]
    fig, axis = plt.subplots(figsize=SINGLE)
    for workload in ("W1", "W2"):
        for release, linestyle in (("GLOBAL_HOLD", "-"), ("DEP_RELEASE", "--")):
            groups = defaultdict(list)
            for row in rows:
                if (row["source"] == "L2" and row["workload"] == workload
                        and row["release"] == release):
                    groups[float(row["delta_over_job"])].append(float(row["overhead_fraction"]) * 100.0)
            xs = sorted(groups)
            ys = [statistics.mean(groups[x]) for x in xs]
            axis.plot(xs, ys, linestyle=linestyle, marker="o", markersize=2.8,
                      color=COLORS[workload],
                      label=f"{workload} {release}")
    p1 = [row for row in rows if row["source"] == "P1-F"]
    if p1:
        axis.plot([float(row["delta_over_job"]) for row in p1],
                  [float(row["overhead_fraction"]) * 100.0 for row in p1],
                  linestyle="none", marker="x", color="black", markersize=4.5,
                  label="P1-F measured")
    l3 = [row for row in rows if row["source"] == "L3"]
    if l3:
        axis.plot([float(row["delta_over_job"]) for row in l3],
                  [float(row["overhead_fraction"]) * 100.0 for row in l3],
                  linestyle="none", marker="D", markerfacecolor="none",
                  markeredgecolor="black", markersize=4.5, label="E2E-9 L3")
    axis.axhline(0.0, color="#999999", linewidth=0.5)
    axis.set_xlabel(r"Release delay $\Delta/T_{\mathrm{job}}$")
    axis.set_ylabel("Steady-state overhead (%)")
    axis.set_ylim(bottom=-2)
    axis.legend(loc="upper left", frameon=False, fontsize=6.2)
    save_figure(
        fig, "p2f3_release", FIGURES, manifest, [path],
        {"workloads": ["W1", "W2"], "fault": "none", "n_tiles": 8},
        len(rows), "mean per workload/release/ratio; L3 release points",
        "P2-F3. No-fault publication overhead with PIM/host mode mutual exclusion. "
        "Host consumes only final outputs, so t_readable=max(t_release,t_exit); "
        "DEP_RELEASE is never negative. GLOBAL_HOLD uses H=Delta; DEP_RELEASE "
        "uses Delta. Black x are Phase 1 P1-F measured points and diamonds are "
        "E2E-9 L3 measurements (GLOBAL_HOLD H=T) with the calibrated host-read "
        "cost added so they share the L2 host-inclusive basis.")


def plot_f4(manifest: list) -> None:
    path = FIGDATA / "p2f4_radius.csv"
    rows = read_csv(path)
    policies = ["B2", "B2R-0", "B2R-1", "B2R-2", "B0"]
    rows = sorted(rows, key=lambda row: policies.index(row["policy"]))
    policy_colors = ["#009E73", "#56B4E9", "#0072B2", "#CC79A7", "#5A5A5A"]
    fig, axes = plt.subplots(1, 2, figsize=DOUBLE)
    y = np.arange(len(rows))
    axes[0].barh(y, [float(row["replay_fraction"]) * 100.0 for row in rows],
                 color=policy_colors, height=0.62)
    axes[0].set_yticks(y)
    axes[0].set_yticklabels([row["policy"] for row in rows])
    axes[0].invert_yaxis()
    axes[0].set_xlabel("Replayed tiles (% of job)")
    axes[0].set_xlim(0, 105)
    width = 0.36
    mix_a = [float(row["expected_coverage_mix_a"]) * 100.0 for row in rows]
    mix_b = [float(row["expected_coverage_mix_b"]) * 100.0 for row in rows]
    axes[1].barh(y - width / 2, mix_a, height=width, color="#56B4E9",
                 label="MIX-A (near)")
    axes[1].barh(y + width / 2, mix_b, height=width, color="#D55E00",
                 label="MIX-B (far)")
    axes[1].set_yticks(y)
    axes[1].set_yticklabels([])
    axes[1].invert_yaxis()
    axes[1].set_xlabel("Expected silent-error coverage (%)")
    axes[1].set_xlim(0, 100)
    axes[1].legend(loc="lower right", frameon=False, fontsize=6.3)
    panel_label(axes[0], "(a)")
    panel_label(axes[1], "(b)")
    save_figure(
        fig, "p2f4_radius", FIGURES, manifest, [path],
        {"error_class": "CE", "n_tiles": 32, "policies": policies},
        len(rows), "mean replay fraction and expected coverage under MIX-A/B",
        "P2-F4. Radius policies at n_tiles=32. (a) Replayed tiles as a fraction "
        "of the job. (b) Expected silent-error coverage under two illustrative "
        "distance distributions: MIX-A same_row/±1/±2/far = 0.20/0.70/0.08/0.02, "
        "MIX-B = 0.10/0.50/0.30/0.10. The distributions are ASSUMED and do not "
        "correspond to a measured DRAM device.")


def plot_f5(manifest: list) -> None:
    path = FIGDATA / "p2f5_status.csv"
    rows = read_csv(path)
    scenarios = ["NORMAL_UE", "CE", "DELTA_SHORT_PAGE_0.5",
                 "DELTA_SHORT_BANK_0.5", "DELTA_FULL_1.0",
                 "SAME_CYCLE_RELEASE", "CHECKPOINT_MISSING",
                 "LEDGER_TRUNCATED", "REPORT_OVERFLOW"]
    statuses = ["SELECTIVE_RECOVERED", "SOURCE_REPAIRED_ONLY",
                "PARTIAL_WITH_ESCAPE", "WHOLE_JOB_FALLBACK",
                "OUTSIDE_WINDOW_NO_TRUSTED_INPUT"]
    groups = defaultdict(dict)
    totals = defaultdict(int)
    for row in rows:
        groups[row["scenario"]][row["status"]] = int(row["count"])
        totals[row["scenario"]] = int(row["total"])
    fig, axis = plt.subplots(figsize=(7.16, 2.8))
    left = np.zeros(len(scenarios))
    for status in statuses:
        values = np.array([100.0 * groups[scenario].get(status, 0)
                           / max(1, totals[scenario]) for scenario in scenarios])
        axis.barh(np.arange(len(scenarios)), values, left=left,
                  color=STATUS_COLORS[status], label=status, height=0.66,
                  edgecolor="white", linewidth=0.35)
        left += values
    axis.set_yticks(np.arange(len(scenarios)))
    axis.set_yticklabels([scenario.replace("_", " ") for scenario in scenarios],
                         fontsize=6.2)
    axis.invert_yaxis()
    axis.set_xlabel("Share of runs (%)")
    axis.set_xlim(0, 100)
    axis.text(100.5, 3, "21/30 silent wrong", fontsize=5.8, va="center",
              color="#D55E00")
    axis.legend(loc="upper center", bbox_to_anchor=(0.5, 1.22), ncol=3,
                frameon=False, fontsize=5.9)
    save_figure(
        fig, "p2f5_status", FIGURES, manifest, [path],
        {"seeds": 30, "workload": "W1", "n_tiles": 32},
        len(rows), "share of P2Status per scenario; B0 excluded",
        "P2-F5. P2Status distribution over 30 seeds. DELTA_FULL_1.0 uses the true "
        "delay and now recovers selectively; DELTA_SHORT_PAGE_0.5 is fixed by "
        "BIRTH windows; DELTA_SHORT_BANK_0.5 still has 21/30 runs with wrong "
        "output despite SELECTIVE_RECOVERED, which is the remaining bank-granularity "
        "limitation. B0 is excluded from the status distribution.")


def plot_f6(manifest: list) -> None:
    path = FIGDATA / "p2f6_validation.csv"
    rows = read_csv(path)
    summary_path = ROOT / "results/phase2/v21/l3/p2f6_summary.json"
    summary = json.loads(summary_path.read_text()) if summary_path.exists() else {}
    fig, axes = plt.subplots(1, 2, figsize=DOUBLE)
    for axis, metric, ylabel, panel in (
            (axes[0], "recovery_cycles_excl_resume",
             "L2 predicted recovery excl. resume", "(a)"),
            (axes[1], "fault_overhead_cycles",
             "L2 predicted fault overhead", "(b)")):
        all_values = []
        for workload in ("W1", "W2"):
            subset = [row for row in rows
                      if row["workload"] == workload and row["metric"] == metric]
            xs = [num(row["l3_value"]) for row in subset]
            ys = [num(row["l2_value"]) for row in subset]
            pairs = [(x, y) for x, y in zip(xs, ys) if x and y and x > 0]
            if not pairs:
                continue
            all_values += [value for pair in pairs for value in pair]
            axis.scatter([x for x, _ in pairs], [y for _, y in pairs], s=7,
                         alpha=0.45, color=COLORS[workload], label=workload,
                         linewidths=0)
        if all_values:
            low = min(all_values) * 0.8
            high = max(all_values) * 1.25
            axis.plot([low, high], [low, high], linestyle="--", color="#666666",
                      linewidth=0.7)
            axis.set_xscale("log")
            axis.set_yscale("log")
            axis.set_xlim(low, high)
            axis.set_ylim(low, high)
        key = "W1/" + metric
        mape_w1 = summary.get("by_workload_metric", {}).get(key, {}).get("mape_percent")
        key = "W2/" + metric
        mape_w2 = summary.get("by_workload_metric", {}).get(key, {}).get("mape_percent")
        text = []
        if mape_w1 is not None:
            text.append(f"W1 MAPE {mape_w1:.1f}%")
        if mape_w2 is not None:
            text.append(f"W2 MAPE {mape_w2:.1f}%")
        axis.text(0.03, 0.97, "\n".join(text), transform=axis.transAxes,
                  va="top", fontsize=6.5)
        axis.set_xlabel("L3 measured (cycles)")
        axis.set_ylabel(ylabel)
        axis.legend(loc="lower right", frameon=False, markerscale=1.8)
        panel_label(axis, panel)
    save_figure(
        fig, "p2f6_validation", FIGURES, manifest, [path],
        {"workloads": ["W1", "W2"], "split": "all L3 fault recoveries"},
        len(rows), "per run; L2 prediction vs L3 measurement",
        "P2-F6. L2 predictions versus all L3 W1/W2 fault-recovery cases. "
        "Calibration uses only no-fault L3 kernel timings at n={8,16,32}; the "
        "validation set is every normal recovery run (NO_REPLAY controls excluded). "
        "(a) recovery_cycles_excl_resume; (b) fault_overhead_cycles. MAPE is "
        "printed per workload; W2 is fully out-of-sample for the per-kernel fit.")


def plot_f7(manifest: list) -> None:
    path = FIGDATA / "p2f7_late.csv"
    rows = read_csv(path)
    workloads = ["W2", "W4_F1", "W4_F4", "W4_F16"]
    fig, axes = plt.subplots(1, 2, figsize=DOUBLE)
    for axis, metric, ylabel, panel in (
            (axes[0], "affected_fraction", "Affected tile operations (%)", "(a)"),
            (axes[1], "fault_overhead_cycles", "Fault overhead (cycles)", "(b)")):
        for workload in workloads:
            groups = defaultdict(list)
            for row in rows:
                if (row["source"] == "L2" and row["workload"] == workload
                        and row["policy"] == "B1" and row["window_mode"] == "AUTO"):
                    groups[float(row["delta_over_tk1"])].append(num(row[metric]))
            xs = sorted(groups)
            ys = [statistics.mean([value for value in groups[x] if value is not None])
                  for x in xs]
            if metric == "affected_fraction":
                ys = [value * 100.0 for value in ys]
            axis.plot(xs, ys, marker=MARKERS[workload], color=COLORS[workload],
                      label=workload.replace("_", " "))
        l3 = [row for row in rows if row["source"] == "L3"]
        if l3:
            axis.plot([float(row["delta_over_tk1"]) for row in l3],
                      [float(row[metric]) * (100.0 if metric == "affected_fraction" else 1.0)
                       for row in l3],
                      linestyle="none", marker="*", color="black", markersize=6.5,
                      label="E2E-8 L3")
        b0 = [row for row in rows if row["source"] == "L2" and row["policy"] == "B0"]
        if b0:
            b0_mean = statistics.mean(float(row[metric]) for row in b0)
            if metric == "affected_fraction":
                b0_mean *= 100.0
            axis.axhline(b0_mean, color="#5A5A5A", linestyle=":",
                         linewidth=0.8, label="B0 mean")
        axis.set_xlabel(r"$D/T_{K1}$")
        axis.set_ylabel(ylabel)
        axis.set_xlim(left=0)
        axis.set_ylim(bottom=0)
        if metric == "affected_fraction":
            axis.set_ylim(0, 40)
        panel_label(axis, panel)
    handles, labels_ = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels_, loc="upper center", ncol=6, frameon=False,
               fontsize=5.8, bbox_to_anchor=(0.5, 1.10))
    save_figure(
        fig, "p2f7_late", FIGURES, manifest, [path],
        {"workloads": workloads, "error_class": "UE", "granularity": "page",
         "window_mode": "AUTO"},
        len(rows), "mean per workload/D ratio; B0 gray reference; E2E-8 L3 stars",
        "P2-F7. Late-report sweep at n_tiles=32, page reports, UE, B1 with AUTO "
        "windows. (a) affected tile operations; (b) fault_overhead_cycles. F is "
        "the W4 fan-out. As D grows, more downstream readers have completed and "
        "the forward closure widens; W4_F16 rises fastest. E2E-8 confirms the "
        "same effect in the simulator (MUL_2 enters affected). W4 costs are "
        "ASSUMED from the MUL fit.")


def write_t1(data: Path) -> None:
    rows = read_csv(data / "p2t1_ledger.csv")
    order = {"W1": 0, "W2": 1, "W3_K4": 2, "W3_K16": 3}
    rows.sort(key=lambda row: (order.get(row["workload"], 9), int(row["n_tiles"]),
                               row["release"], float(row["delta_over_job"])))
    lines = ["| Workload | n | Release | Delta/T | Entries | Bytes | Peak held |",
             "|---|---:|---|---:|---:|---:|---:|"]
    for row in rows:
        lines.append(f"| {row['workload']} | {row['n_tiles']} | {row['release']} | "
                     f"{float(row['delta_over_job']):.2f} | {row['entries']} | "
                     f"{row['bytes_est']} | {row['peak_held_items']} |")
    (FIGURES / "p2t1_ledger.md").write_text("\n".join(lines) + "\n")
    tex = [r"\begin{table}[t]", r"\centering",
           r"\begin{tabular}{lrrrrrr}", r"\toprule",
           r"Workload & $n$ & Release & $\Delta/T$ & Entries & Bytes & Peak held \\",
           r"\midrule"]
    for row in rows:
        workload = row["workload"].replace("_", "\\_")
        release = row["release"].replace("_", "\\_")
        tex.append(
            f"{workload} & {row['n_tiles']} & {release} & "
            f"{float(row['delta_over_job']):.2f} & {row['entries']} & "
            f"{row['bytes_est']} & {row['peak_held_items']} " + "\\\\")
    tex += [r"\bottomrule", r"\end{tabular}",
            r"\caption{Phase 2 v2.1 ledger overhead (P2-T1).}", r"\end{table}"]
    (FIGURES / "p2t1_ledger.tex").write_text("\n".join(tex) + "\n")


def write_t2() -> None:
    rows = [
        ("DELTA_FULL_1.0 status", "PARTIAL_WITH_ESCAPE 30/30", "SELECTIVE_RECOVERED 30/30"),
        ("DELTA_SHORT_0.5 (page) status/output", "SOURCE_REPAIRED_ONLY, wrong", "SELECTIVE_RECOVERED, correct 30/30"),
        ("DELTA_SHORT_0.5 (bank) status/output", "not run", "21/30 SELECTIVE wrong, 9/30 fallback correct"),
        ("W1 bank B1 replay fraction", "46.4%", "9.55% (D-averaged)"),
        ("W1 bank B1 / B0 fault overhead", "104.3% (recovery)", "18.1% (fault overhead)"),
        ("E2E-3 restore_bytes / affected", "65,536 B / 5 tiles", "32,768 B / 3 tiles"),
        ("DEP_RELEASE overhead at Delta=0", "-36%", "0.01% (model, no negative)"),
        ("W2 page UE affected fraction", "1.04% (D-averaged)", "0.35% / 1.39% / 1.39% at D=16/256/4096"),
        ("W2 validation MAPE", "22.43% (recovery)", "11.89% (fault overhead) / 21.16% (recovery excl. resume)"),
        ("F2 cases classified MASKED", "not measured", "160/360 = 44.4%"),
        ("B0 status label", "WHOLE_JOB_FALLBACK", "BASELINE_B0"),
        ("L2 cases", "47,946 (report) / 48,666 actual", "59,706"),
        ("L3 runs", "745", "1,120"),
    ]
    lines = ["| Metric | v2 | v2.1 |", "|---|---|---|"]
    lines += [f"| {old} | {a} | {b} |" for old, a, b in rows]
    (FIGURES / "p2t2_comparison.md").write_text("\n".join(lines) + "\n")
    tex = [r"\begin{table}[t]", r"\centering", r"\small",
           r"\begin{tabular}{p{5.2cm}p{3.5cm}p{5.2cm}}", r"\toprule",
           r"Metric & v2 & v2.1 \\", r"\midrule"]
    for old, a, b in rows:
        tex.append(f"{old} & {a} & {b} " + r"\\")
    tex += [r"\bottomrule", r"\end{tabular}",
            r"\caption{Phase 2 v2.1 key-number comparison (P2-T2).}", r"\end{table}"]
    (FIGURES / "p2t2_comparison.tex").write_text("\n".join(tex) + "\n")


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
    plot_f1(manifest)
    plot_f2(manifest)
    plot_f3(manifest)
    plot_f4(manifest)
    plot_f5(manifest)
    plot_f6(manifest)
    plot_f7(manifest)
    write_t1(FIGDATA)
    write_t2()
    (FIGURES / "plot_manifest.json").write_text(
        json.dumps({"schema_version": 1, "figures": manifest}, indent=2,
                   sort_keys=True) + "\n")
    captions = ["# Phase 2 v2.1 figure captions", ""]
    for entry in manifest:
        captions += [f"## {entry['figure']}", "", entry["caption"], "",
                     f"- Sample rows: {entry['sample_count']}",
                     "- Source: `results/phase2/v21/figure_data/`.",
                     "- ASSUMED: W4/MUL/RELU cost fits are L3-derived but not "
                     "hardware-measured; MIX-A/MIX-B are illustrative; single "
                     "channel; host copy/OS overhead not modelled.", ""]
    (FIGURES / "captions.md").write_text("\n".join(captions))
    print(f"FIGURES={len(manifest)}")
    for entry in manifest:
        print(entry["figure"], entry["sample_count"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
