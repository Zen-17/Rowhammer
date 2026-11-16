#!/usr/bin/env python3
"""Compute paper statistics and render the six required figures.

Only ``results/figure_data/*.csv`` is used as plotting input.  The script writes
bootstrap/Wilson statistics, a plot manifest with input/output hashes, and
PDF/SVG/300-dpi PNG figures.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import statistics
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from PIL import Image

plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 9,
    "axes.labelsize": 9, "axes.titlesize": 9,
    "xtick.labelsize": 8, "ytick.labelsize": 8, "legend.fontsize": 8,
    "pdf.fonttype": 42, "ps.fonttype": 42,
    "axes.spines.top": False, "axes.spines.right": False,
    "savefig.dpi": 300, "figure.dpi": 120,
})
COLORS = ["#0072B2", "#D55E00", "#009E73", "#666666"]
STAT_SEED = 20261115
BOOTSTRAP_N = 10000


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_csv(path: Path) -> list[dict]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def truth(value: str) -> bool:
    return str(value).strip().lower() == "true"


def number(value: str) -> float | None:
    if value is None or value == "":
        return None
    return float(value)


def bootstrap_ci(values: list[float], n_boot: int = BOOTSTRAP_N,
                 seed: int = STAT_SEED) -> tuple[float | None, float | None]:
    if len(values) < 2:
        return None, None
    rng = np.random.Generator(np.random.PCG64(seed))
    data = np.asarray(values, dtype=float)
    means = np.empty(n_boot)
    for i in range(n_boot):
        means[i] = data[rng.integers(0, len(data), len(data))].mean()
    return float(np.quantile(means, 0.025)), float(np.quantile(means, 0.975))


def wilson(k: int, n: int) -> tuple[float | None, float | None, float | None]:
    if n == 0:
        return None, None, None
    z = 1.96
    p = k / n
    denom = 1.0 + z * z / n
    center = (p + z * z / (2 * n)) / denom
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom
    return p, max(0.0, center - half), min(1.0, center + half)


def add_continuous(rows: list[dict], figure: str, metric: str, group: str, config: str,
                   values: list[float], ci_method: str = "bootstrap_mean_95") -> None:
    values = [v for v in values if v is not None]
    if not values:
        return
    low, high = bootstrap_ci(values) if ci_method.startswith("bootstrap") else (None, None)
    rows.append({
        "stat_id": f"{figure}_{metric}_{group}_{config}".replace(" ", "_"),
        "figure": figure, "metric": metric, "group": group, "config": config,
        "n_samples": len(values), "n_seeds": len(values) if "bootstrap" in ci_method else "",
        "numerator": "", "denominator": "",
        "mean": statistics.mean(values), "median": statistics.median(values),
        "min": min(values), "max": max(values),
        "ci_low": low if low is not None else "", "ci_high": high if high is not None else "",
        "ci_method": ci_method,
    })


def add_proportion(rows: list[dict], figure: str, metric: str, group: str, config: str,
                   k: int, n: int) -> None:
    p, low, high = wilson(k, n)
    if p is None:
        return
    rows.append({
        "stat_id": f"{figure}_{metric}_{group}_{config}".replace(" ", "_"),
        "figure": figure, "metric": metric, "group": group, "config": config,
        "n_samples": n, "n_seeds": "",
        "numerator": k, "denominator": n,
        "mean": p, "median": p, "min": "", "max": "",
        "ci_low": low, "ci_high": high, "ci_method": "wilson_95",
    })


def save_figure(fig, name: str, figures: Path, manifest: list[dict], inputs: list[Path],
                filters: dict, sample_count: int, aggregation: str) -> None:
    outputs = []
    for suffix in ("pdf", "svg", "png"):
        path = figures / f"{name}.{suffix}"
        fig.savefig(path, bbox_inches="tight", facecolor="white")
        outputs.append({"path": str(path.relative_to(figures.parent)), "sha256": sha256(path)})
    gray_dir = figures / "gray"
    gray_dir.mkdir(exist_ok=True)
    gray_path = gray_dir / f"{name}.png"
    Image.open(figures / f"{name}.png").convert("L").save(gray_path)
    outputs.append({"path": str(gray_path.relative_to(figures.parent)), "sha256": sha256(gray_path)})
    plt.close(fig)
    manifest.append({
        "figure": name,
        "inputs": [{"path": str(path.relative_to(figures.parent)), "sha256": sha256(path)}
                   for path in inputs],
        "columns": sorted({key for path in inputs for key in read_csv(path)[0].keys()}),
        "filters": filters,
        "sample_count": sample_count,
        "aggregation": aggregation,
        "outputs": outputs,
    })


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--results", default="results")
    parser.add_argument("--out", default="figures")
    args = parser.parse_args()
    results = Path(args.results).resolve()
    figures = Path(args.out).resolve()
    figures.mkdir(parents=True, exist_ok=True)
    data = results / "figure_data"
    fig1_path = data / "fig1_path_coverage.csv"
    fig2_path = data / "fig2_report_latency.csv"
    fig2e_path = data / "fig2_report_events.csv"
    fig3_path = data / "fig3_control_window.csv"
    fig4_path = data / "fig4_quiescence.csv"
    fig5_path = data / "fig5_restore.csv"
    fig6_path = data / "fig6_cost.csv"
    fig6r_path = data / "fig6_robustness.csv"
    required = [fig1_path, fig2_path, fig2e_path, fig3_path, fig4_path, fig5_path, fig6_path,
                fig6r_path]
    for path in required:
        if not path.exists():
            raise SystemExit(f"missing figure input: {path}")
    fig1 = read_csv(fig1_path)
    fig2 = read_csv(fig2_path)
    fig2e = read_csv(fig2e_path)
    fig3 = read_csv(fig3_path)
    fig4 = read_csv(fig4_path)
    fig5 = read_csv(fig5_path)
    fig6 = read_csv(fig6_path)
    fig6r = read_csv(fig6r_path)
    stats: list[dict] = []
    plot_manifest: list[dict] = []

    # Fig.1 path coverage: 4 profiles x 4 paths, F1/F2 panels.
    profiles = ["NATIVE", "LOCAL_VISIBLE", "LOCAL_HIDDEN", "HOST_ONLY"]
    paths = ["HREAD", "PREAD", "PWRITE_HREAD", "PWRITE_PREAD"]
    fig1_groups = defaultdict(list)
    for row in fig1:
        fig1_groups[(row["profile"], row["path"], row["fault"])].append(row)
    for (profile, path, fault), rows in sorted(fig1_groups.items()):
        correct = sum(1 for row in rows if truth(row["output_correct"]))
        reported = sum(1 for row in rows
                       if int(float(row["delivered_reports"])) > 0
                       or int(float(row["internal_ce"])) > 0
                       or int(float(row["internal_ue"])) > 0)
        add_proportion(stats, "fig01", "corrected_fraction", f"{profile}/{path}",
                       fault, correct, len(rows))
        add_proportion(stats, "fig01", "reported_fraction", f"{profile}/{path}",
                       fault, reported, len(rows))
    fig, axes = plt.subplots(1, 2, figsize=(7.0, 3.1))
    for axis, fault in zip(axes, ["F1", "F2"]):
        matrix = np.zeros((len(profiles), len(paths)))
        labels: list[list[str]] = []
        for i, profile in enumerate(profiles):
            label_row = []
            for j, path in enumerate(paths):
                rows = fig1_groups.get((profile, path, fault), [])
                correct = sum(1 for row in rows if truth(row["output_correct"]))
                reported = sum(1 for row in rows
                               if int(float(row["delivered_reports"])) > 0
                               or int(float(row["internal_ce"])) > 0
                               or int(float(row["internal_ue"])) > 0)
                invalid = sum(1 for row in rows if not truth(row["output_correct"]))
                uncovered = sum(1 for row in rows
                                if not truth(row["output_correct"])
                                and int(float(row["delivered_reports"])) == 0
                                and int(float(row["internal_ce"])) == 0
                                and int(float(row["internal_ue"])) == 0
                                and not truth(row["format_invalid"]))
                matrix[i, j] = correct / len(rows) if rows else np.nan
                label_row.append(f"C{correct} R{reported}\nI{invalid} U{uncovered}")
            labels.append(label_row)
        image = axis.imshow(matrix, cmap="Blues", vmin=0, vmax=1, aspect="auto")
        axis.set_xticks(range(len(paths)))
        axis.set_xticklabels(paths, rotation=20, ha="right")
        axis.set_yticks(range(len(profiles)))
        axis.set_yticklabels(profiles)
        axis.set_title("F1 single-bit" if fault == "F1" else "F2 double-bit")
        for i in range(len(profiles)):
            for j in range(len(paths)):
                axis.text(j, i, labels[i][j], ha="center", va="center", fontsize=6.5,
                          color="black" if matrix[i, j] < 0.65 else "white")
        axis.set_xlabel("data path")
    axes[1].set_yticklabels([])
    axes[1].tick_params(axis="y", length=0)
    fig.subplots_adjust(wspace=0.28)
    cbar = fig.colorbar(image, ax=axes.ravel().tolist(), shrink=0.82, pad=0.02)
    cbar.set_label("corrected fraction (3 banks)")
    save_figure(fig, "fig01_path_coverage", figures, plot_manifest,
                [fig1_path], {"faults": ["F1", "F2"], "banks": [0, 3, 15]},
                len(fig1), "per profile/path/fault: C/R/I/U counts over 3 banks; color=corrected fraction")

    # Fig.2 report latency: configured D and burst cumulative delivery.
    fig2_groups = defaultdict(list)
    for row in fig2:
        if row["path"] != "HREAD":
            continue
        fig2_groups[(row["granularity"], int(float(row["service_interval_cycles"])))].append(row)
    delays = [0, 1, 16, 256, 4096]
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.0, 3.1))
    markers = ["o", "s", "^", "D"]
    for index, ((granularity, interval), rows) in enumerate(sorted(fig2_groups.items())):
        means = []
        for delay in delays:
            selected = [float(row["report_latency_mean"]) for row in rows
                        if int(float(row["transport_delay_cycles"])) == delay]
            means.append(statistics.mean(selected) if selected else np.nan)
        ax1.plot(range(len(delays)), means, marker=markers[index % len(markers)],
                 color=COLORS[index % len(COLORS)], linestyle="-" if interval == 1 else "--",
                 label=f"{granularity}, I={interval}")
        for delay, value in zip(delays, means):
            if value is not None and not np.isnan(value):
                add_continuous(stats, "fig02", "report_latency", f"{granularity}/I{interval}",
                               f"D{delay}", [value], ci_method="none_deterministic")
    ax1.set_yscale("log")
    ax1.set_xticks(range(len(delays)))
    ax1.set_xticklabels([str(value) for value in delays])
    ax1.set_xlabel("configured D (cycles, categorical)")
    ax1.set_ylabel("observed report latency (cycles, log)")
    ax1.set_title("(a) single-event latency")
    ax1.legend(loc="upper left")
    ax1.grid(True, which="both", alpha=0.2)
    event_groups = defaultdict(list)
    for row in fig2e:
        event_groups[int(float(row["bank"]))].append(row)
    for index, (bank, rows) in enumerate(sorted(event_groups.items())):
        rows = sorted(rows, key=lambda row: int(float(row["report_index"])))
        ax2.plot([int(float(row["report_cycle"])) for row in rows],
                 [int(float(row["cumulative_reports"])) for row in rows],
                 marker=markers[index % len(markers)], color=COLORS[index % len(COLORS)],
                 linewidth=1.0, label=f"bank {bank}")
    ax2.set_xlabel("report delivery cycle")
    ax2.set_ylabel("cumulative delivered reports")
    ax2.set_title("(b) 32-read burst, I=8")
    ax2.legend(loc="lower right")
    ax2.grid(True, alpha=0.2)
    save_figure(fig, "fig02_report_latency", figures, plot_manifest,
                [fig2_path, fig2e_path], {"path": "HREAD", "burst_banks": [0, 3, 15]},
                len(fig2) + len(fig2e),
                "single-event mean over banks; burst cumulative counts from reports.csv")

    # Fig.3 control window heatmaps, 2 consumers x 2 sizes.
    main3 = [row for row in fig3 if not truth(row["control"])]
    delay_values = sorted({float(row["delay_ratio"]) for row in main3})
    hold_values = sorted({float(row["hold_ratio"]) for row in main3})
    timely_groups = defaultdict(list)
    for row in main3:
        timely_groups[(int(row["n_tiles"]), row["consumer"],
                       float(row["delay_ratio"]), float(row["hold_ratio"]))].append(row)
    for key, rows in sorted(timely_groups.items()):
        n_tiles, consumer, delay, hold = key
        timely = sum(1 for row in rows if row["outcome_class"] == "TIMELY_CONTROL")
        add_proportion(stats, "fig03", "timely_control_fraction",
                       f"nt{n_tiles}/{consumer}", f"D{delay}/H{hold}", timely, len(rows))
        changed = sum(1 for row in rows if truth(row["output_changed"]))
        consumed = sum(1 for row in rows if truth(row["consumed"]))
        add_proportion(stats, "fig03", "output_changed_fraction",
                       f"nt{n_tiles}/{consumer}", f"D{delay}/H{hold}", changed, consumed)
    fig, axes = plt.subplots(2, 2, figsize=(7.0, 5.4))
    for axis, n_tiles, consumer in [
        (axes[0, 0], 1, "HOST"), (axes[0, 1], 1, "PIM"),
        (axes[1, 0], 8, "HOST"), (axes[1, 1], 8, "PIM")]:
        matrix = np.full((len(delay_values), len(hold_values)), np.nan)
        for i, delay in enumerate(delay_values):
            for j, hold in enumerate(hold_values):
                rows = timely_groups.get((n_tiles, consumer, delay, hold), [])
                if rows:
                    matrix[i, j] = sum(1 for row in rows
                                       if row["outcome_class"] == "TIMELY_CONTROL") / len(rows)
        image = axis.imshow(matrix, cmap="Blues", vmin=0, vmax=1, aspect="auto")
        axis.set_xticks(range(len(hold_values)))
        axis.set_xticklabels([f"{value:g}" for value in hold_values])
        axis.set_yticks(range(len(delay_values)))
        axis.set_yticklabels([f"{value:g}" for value in delay_values])
        if n_tiles == 8:
            axis.set_xlabel("H/T_ref")
        axis.set_ylabel("D/T_ref")
        axis.set_title(f"n_tiles={n_tiles}, {consumer} consumer")
        for i in range(len(delay_values)):
            for j in range(len(hold_values)):
                if not np.isnan(matrix[i, j]):
                    axis.text(j, i, f"{100 * matrix[i, j]:.0f}%", ha="center", va="center",
                              fontsize=7.5, color="black" if matrix[i, j] < 0.65 else "white")
    fig.subplots_adjust(hspace=0.38, wspace=0.22)
    cbar = fig.colorbar(image, ax=axes.ravel().tolist(), shrink=0.82, pad=0.02)
    cbar.set_label("timely-control fraction")
    save_figure(fig, "fig03_control_window", figures, plot_manifest,
                [fig3_path], {"profile": "LOCAL_VISIBLE", "control": False},
                len(main3), "fraction TIMELY_CONTROL over 30 seeds per cell; no output changed observed")

    # Fig.4 quiescence and independent channel progress.
    scenario_order = ["SYNTHETIC_BEFORE", "SYNTHETIC_QUEUE", "SYNTHETIC_INFLIGHT",
                      "SYNTHETIC_NEAR"]
    limits = sorted({int(float(row["feeder_limit"])) for row in fig4
                     if row["scenario"].startswith("SYNTHETIC")})
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.0, 3.1))
    for index, scenario in enumerate(scenario_order):
        rows = [row for row in fig4 if row["scenario"] == scenario]
        means = []
        for limit in limits:
            selected = [float(row["quiesce_cycles"]) for row in rows
                        if int(float(row["feeder_limit"])) == limit]
            means.append(statistics.mean(selected) if selected else np.nan)
        ax1.plot(range(len(limits)), means, marker=markers[index % len(markers)],
                 color=COLORS[index % len(COLORS)], label=scenario.replace("SYNTHETIC_", ""))
        for limit, value in zip(limits, means):
            add_continuous(stats, "fig04", "quiesce_cycles", scenario, f"K{limit}",
                           [value], ci_method="none_deterministic")
    actual = [row for row in fig4 if row["scenario"] == "ACTUAL_CE"]
    actual_means = []
    for limit in limits:
        selected = [float(row["quiesce_cycles"]) for row in actual
                    if int(float(row["feeder_limit"])) == limit]
        actual_means.append(statistics.mean(selected) if selected else np.nan)
    ax1.plot(range(len(limits)), actual_means, marker="x", color="black",
             linestyle=":", linewidth=1.2, label="ACTUAL_CE (mean L)")
    ax1.set_xticks(range(len(limits)))
    ax1.set_xticklabels([str(value) for value in limits])
    ax1.set_xlabel("feeder outstanding limit K")
    ax1.set_ylabel("quiescence latency (cycles)")
    ax1.set_title("(a) drain latency by trigger")
    ax1.legend(loc="upper left")
    ax1.grid(True, alpha=0.2)
    two = [row for row in fig4 if row["scenario"] == "TWO_CHANNEL"]
    if two:
        row = two[0]
        values = [float(row["data_delta_after_ack"]), float(row["other_channel_data_delta"])]
        bars = ax2.bar(["channel 0\n(paused)", "channel 1\n(unpaused)"], values,
                       color=[COLORS[1], COLORS[0]])
        for bar, value in zip(bars, values):
            ax2.text(bar.get_x() + bar.get_width() / 2, value, f"{int(value)}",
                     ha="center", va="bottom")
        ax2.set_ylabel("data accesses after ACK (delta)")
        ax2.set_title("(b) independent-channel progress")
        ax2.grid(True, axis="y", alpha=0.2)
    save_figure(fig, "fig04_quiescence", figures, plot_manifest,
                [fig4_path], {"scenarios": scenario_order + ["ACTUAL_CE", "TWO_CHANNEL"]},
                len(fig4), "synthetic means over 3 K values; actual CE mean over L; two-channel delta")

    # Fig.5 restore stage decomposition.
    fig5_groups = defaultdict(list)
    for row in fig5:
        fig5_groups[int(float(row["n_tiles"]))].append(row)
    sizes = sorted(fig5_groups)
    restore_means, reexec_means, recovery_values, recovery_ci = [], [], [], []
    for size in sizes:
        rows = fig5_groups[size]
        restore_means.append(statistics.mean(float(row["restore_cycles"]) for row in rows))
        reexec_means.append(statistics.mean(float(row["reexec_cycles"]) for row in rows))
        values = [float(row["recovery_cycles"]) for row in rows]
        recovery_values.append(statistics.mean(values))
        low, high = bootstrap_ci(values)
        recovery_ci.append((low, high))
        add_continuous(stats, "fig05", "restore_cycles", f"nt{size}", "all_seeds",
                       [float(row["restore_cycles"]) for row in rows])
        add_continuous(stats, "fig05", "reexec_cycles", f"nt{size}", "all_seeds",
                       [float(row["reexec_cycles"]) for row in rows])
        add_continuous(stats, "fig05", "recovery_cycles", f"nt{size}", "all_seeds", values)
    fig, ax = plt.subplots(figsize=(3.35, 3.0))
    x = np.arange(len(sizes))
    ax.bar(x, restore_means, width=0.55, color=COLORS[0], label="input restore")
    ax.bar(x, reexec_means, width=0.55, bottom=restore_means, color=COLORS[1],
           label="W1 re-execution")
    errors = np.array([[recovery_values[i] - recovery_ci[i][0],
                        recovery_ci[i][1] - recovery_values[i]] for i in range(len(sizes))]).T
    ax.errorbar(x, recovery_values, yerr=errors, fmt="ko", capsize=3,
                label="ACK→publish (95% bootstrap CI)")
    for index, size in enumerate(sizes):
        rows = fig5_groups[size]
        jitter = np.linspace(-0.08, 0.08, len(rows))
        ax.scatter(np.full(len(rows), x[index]) + jitter,
                   [float(row["recovery_cycles"]) for row in rows], s=8, color="black",
                   alpha=0.5, zorder=3)
    ax.set_xticks(x)
    ax.set_xticklabels([str(size) for size in sizes])
    ax.set_xlabel("n_tiles")
    ax.set_ylabel("recovery latency (cycles)")
    ax.set_title("Trusted-input recovery\npositive 20/20; expected negatives 7/7 rejected")
    ax.legend(loc="upper left")
    ax.grid(True, axis="y", alpha=0.2)
    save_figure(fig, "fig05_restore", figures, plot_manifest,
                [fig5_path], {"positive_cases": 20, "n_tiles": sizes},
                len(fig5), "mean stages over 10 seeds; bootstrap 95% CI on ACK→publish")

    # Fig.6 cost overhead and space components.
    policy_order = ["LOCAL_VISIBLE_H0", "LOCAL_VISIBLE_H025T", "LOCAL_VISIBLE_H1T"]
    config_order = [(1, "HOST"), (1, "PIM"), (8, "HOST"), (8, "PIM")]
    cost_groups = defaultdict(list)
    for row in fig6:
        if row["policy"] == "EXT_OFF":
            continue
        cost_groups[(row["policy"], int(float(row["n_tiles"])), row["consumer"])].append(
            float(row["overhead_fraction"]))
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.0, 3.2))
    width = 0.18
    for index, (n_tiles, consumer) in enumerate(config_order):
        means, lows, highs = [], [], []
        for policy in policy_order:
            values = cost_groups.get((policy, n_tiles, consumer), [])
            if values:
                mean = statistics.mean(values)
                low, high = bootstrap_ci(values)
                means.append(mean)
                lows.append(max(0.0, mean - low) if low is not None else 0.0)
                highs.append(max(0.0, high - mean) if high is not None else 0.0)
                add_continuous(stats, "fig06", "overhead_fraction",
                               f"nt{n_tiles}/{consumer}", policy, values)
            else:
                means.append(0)
                lows.append(0)
                highs.append(0)
        positions = np.arange(len(policy_order)) + (index - 1.5) * width
        ax1.bar(positions, [100 * value for value in means], width=width,
                yerr=[lows, highs], capsize=2, color=COLORS[index],
                label=f"nt{n_tiles} {consumer}")
    ax1.set_xticks(range(len(policy_order)))
    ax1.set_xticklabels(["H0", "H0.25T", "H1T"])
    ax1.set_xlabel("policy")
    ax1.set_ylabel("fault-free overhead (%)")
    ax1.set_title("(a) paired simulation-cycle overhead")
    ax1.legend(loc="upper left", ncol=2)
    ax1.grid(True, axis="y", alpha=0.2)
    space_rows = [row for row in fig6 if row["policy"] == "LOCAL_VISIBLE_H1T"]
    space_by_size = {}
    for row in space_rows:
        size = int(float(row["n_tiles"]))
        if size not in space_by_size:
            space_by_size[size] = {
                "checkpoint": float(row["checkpoint_bytes"]) / 1024,
                "held output": float(row["peak_held_output_bytes"]) / 1024,
                "parity": float(row["parity_bytes"]) / 1024,
                "metadata": float(row["runtime_metadata_bytes"]) / 1024,
            }
    sizes = sorted(space_by_size)
    components = ["checkpoint", "held output", "parity", "metadata"]
    bottoms = np.zeros(len(sizes))
    for index, component in enumerate(components):
        values = np.array([space_by_size[size][component] for size in sizes])
        ax2.bar(range(len(sizes)), values, bottom=bottoms, width=0.55,
                color=COLORS[index], label=component)
        bottoms += values
    for index, size in enumerate(sizes):
        add_continuous(stats, "fig06", "space_checkpoint_kib", f"nt{size}", "checkpoint",
                       [space_by_size[size]["checkpoint"]], ci_method="none_deterministic")
        add_continuous(stats, "fig06", "space_held_kib", f"nt{size}", "held_output",
                       [space_by_size[size]["held output"]], ci_method="none_deterministic")
        add_continuous(stats, "fig06", "space_parity_kib", f"nt{size}", "parity",
                       [space_by_size[size]["parity"]], ci_method="none_deterministic")
        add_continuous(stats, "fig06", "space_metadata_kib", f"nt{size}", "metadata",
                       [space_by_size[size]["metadata"]], ci_method="none_deterministic")
        ax2.text(index, bottoms[index], f"{bottoms[index]:.1f} KiB", ha="center", va="bottom",
                 fontsize=7.5)
    ax2.set_xticks(range(len(sizes)))
    ax2.set_xticklabels([str(size) for size in sizes])
    ax2.set_xlabel("n_tiles")
    ax2.set_ylabel("space (KiB)")
    ax2.set_title("(b) resident control and held output")
    ax2.legend(loc="upper left")
    ax2.grid(True, axis="y", alpha=0.2)
    save_figure(fig, "fig06_cost", figures, plot_manifest,
                [fig6_path], {"policy": policy_order, "paired_by": "seed"},
                len(fig6), "paired EXT_OFF per seed; bootstrap 95% CI over 30 seeds")

    # Supplementary robustness figure.
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.0, 3.0))
    robust0 = [row for row in fig6r if row["drop_rate"] == "0"]
    robust1 = [row for row in fig6r if row["drop_rate"] == "0.1"]
    qs = sorted({int(float(row["queue_capacity"])) for row in fig6r})
    intervals = sorted({int(float(row["service_interval_cycles"])) for row in fig6r})
    for index, q in enumerate(qs):
        means = []
        for interval in intervals:
            rows = [row for row in robust0 if int(float(row["queue_capacity"])) == q
                    and int(float(row["service_interval_cycles"])) == interval]
            means.append(float(rows[0]["delivered_reports"]) / 32 if rows else np.nan)
        ax1.plot(range(len(intervals)), means, marker=markers[index % len(markers)],
                 color=COLORS[index % len(COLORS)], label=f"Q={q}")
    ax1.set_xticks(range(len(intervals)))
    ax1.set_xticklabels([str(value) for value in intervals])
    ax1.set_xlabel("service interval I (cycles)")
    ax1.set_ylabel("delivered fraction")
    ax1.set_title("(a) drop=0")
    ax1.legend(loc="lower left")
    ax1.grid(True, alpha=0.2)
    for index, q in enumerate(qs):
        means = []
        for interval in intervals:
            rows = [row for row in robust1 if int(float(row["queue_capacity"])) == q
                    and int(float(row["service_interval_cycles"])) == interval]
            means.append(float(rows[0]["delivered_reports"]) / 32 if rows else np.nan)
        ax2.plot(range(len(intervals)), means, marker=markers[index % len(markers)],
                 color=COLORS[index % len(COLORS)], label=f"Q={q}")
    ax2.set_xticks(range(len(intervals)))
    ax2.set_xticklabels([str(value) for value in intervals])
    ax2.set_xlabel("service interval I (cycles)")
    ax2.set_ylabel("delivered fraction")
    ax2.set_title("(b) drop=0.1 (3 gap injections)")
    ax2.legend(loc="lower left")
    ax2.grid(True, alpha=0.2)
    save_figure(fig, "figS1_robustness", figures, plot_manifest,
                [fig6r_path], {"queue_capacities": qs, "drop_rates": ["0", "0.1"]},
                len(fig6r), "delivered reports / 32 per Q/I/drop case")

    fieldnames = list(stats[0].keys()) if stats else []
    with (results / "statistics.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(stats)
    (results / "statistics.json").write_text(json.dumps({
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "stat_seed": STAT_SEED,
        "bootstrap_resamples": BOOTSTRAP_N,
        "ci_methods": {
            "continuous": "seed-level bootstrap mean, 2.5/97.5 percentiles",
            "proportion": "Wilson 95%",
        },
        "rows": stats,
    }, indent=2) + "\n")
    (figures / "plot_manifest.json").write_text(json.dumps({
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "stat_seed": STAT_SEED,
        "bootstrap_resamples": BOOTSTRAP_N,
        "figures": plot_manifest,
    }, indent=2) + "\n")
    print(f"wrote {len(stats)} statistics rows and {len(plot_manifest)} figures")
    print("G9_PLOT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
