#!/usr/bin/env python3
"""Render supplement Fig. 7, Fig. 8 and Fig. S2 from figure_data CSVs only."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Rectangle
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
FIG_DATA = ROOT / "results/supplement/figure_data"
FIGURES = ROOT / "figures"
MANIFEST = FIGURES / "plot_manifest_supplement.json"
STAT_SEED = 20261115

plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 9,
    "axes.labelsize": 9, "axes.titlesize": 9,
    "xtick.labelsize": 8, "ytick.labelsize": 8, "legend.fontsize": 8,
    "pdf.fonttype": 42, "ps.fonttype": 42,
    "axes.spines.top": False, "axes.spines.right": False,
    "savefig.dpi": 300, "figure.dpi": 120,
})

D_VALUES = [0.0, 0.5, 1.0]
H_VALUES = [0.0, 0.25, 1.0]
FAULTS = [
    ("F3M", "NONE", "F3M"),
    ("F1W_F4S", "SAME_BANK", "F1W SAME_BANK"),
    ("F1W_F4S", "DIFF_BANK", "F1W DIFF_BANK"),
    ("F4S", "NONE", "F4S"),
]
SCENARIOS = ["S1_SINGLE", "S2_DIFF_CW", "S3_DIFF_BURST_SAME_BANK",
             "S4_SAME_CW_TWO_BITS", "S5_DIFF_BANK", "S6_SAME_BIT_RECUR"]
SCENARIO_LABELS = ["S1", "S2", "S3", "S4*", "S5", "S6"]


def read_csv(path: Path) -> list[dict]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def save_figure(fig, name: str, inputs: list[Path], filters: dict,
                sample_count: int, aggregation: str) -> None:
    outputs = []
    for suffix in ("pdf", "svg", "png"):
        path = FIGURES / f"{name}.{suffix}"
        fig.savefig(path, bbox_inches="tight", facecolor="white")
        outputs.append({"path": str(path.relative_to(ROOT)), "sha256": sha256(path)})
    gray_dir = FIGURES / "gray"
    gray_dir.mkdir(exist_ok=True)
    gray_path = gray_dir / f"{name}.png"
    Image.open(FIGURES / f"{name}.png").convert("L").save(gray_path)
    outputs.append({"path": str(gray_path.relative_to(ROOT)), "sha256": sha256(gray_path)})
    plt.close(fig)
    MANIFEST_DATA["figures"].append({
        "figure": name,
        "inputs": [{"path": str(path.relative_to(ROOT)), "sha256": sha256(path)}
                   for path in inputs],
        "columns": sorted({key for path in inputs for key in read_csv(path)[0].keys()}),
        "filters": filters,
        "sample_count": sample_count,
        "aggregation": aggregation,
        "outputs": outputs,
    })


def escape_cell(rows: list[dict], n_tiles: int, consumer: str, fault: str,
                placement: str, delay: float, hold: float):
    selected = [
        row for row in rows
        if int(row["n_tiles"]) == n_tiles
        and row["consumer"] == consumer
        and row["fault_kind"] == fault
        and row["escape_placement"] == placement
        and float(row["delay_ratio"]) == delay
        and float(row["hold_ratio"]) == hold
    ]
    total = len(selected)
    masked = sum(1 for row in selected if row["escape_outcome"] == "MASKED")
    prevented = sum(1 for row in selected if row["escape_outcome"] == "PREVENTED")
    consumed = sum(1 for row in selected if row["escape_outcome"] == "CORRUPT_CONSUMED")
    return consumed, prevented, masked, total - masked, total


def draw_escape_panel(ax, rows: list[dict], n_tiles: int, consumer: str,
                      fault: str, placement: str, title: str,
                      show_xlabel: bool, show_ylabel: bool, font_size: float,
                      show_xticklabels: bool = True, show_yticklabels: bool = True):
    matrix = np.full((len(D_VALUES), len(H_VALUES)), np.nan)
    counts = {}
    for i, delay in enumerate(D_VALUES):
        for j, hold in enumerate(H_VALUES):
            consumed, prevented, masked, n_prime, total = escape_cell(
                rows, n_tiles, consumer, fault, placement, delay, hold)
            counts[(i, j)] = (consumed, prevented, masked, n_prime, total)
            if n_prime > 0:
                matrix[i, j] = consumed / n_prime
    image = ax.imshow(matrix, cmap="Reds", vmin=0, vmax=1, aspect="auto")
    ax.set_xticks(range(len(H_VALUES)))
    ax.set_xticklabels([f"{value:g}" for value in H_VALUES] if show_xticklabels else [])
    ax.set_yticks(range(len(D_VALUES)))
    ax.set_yticklabels([f"{value:g}" for value in D_VALUES] if show_yticklabels else [])
    if show_xlabel:
        ax.set_xlabel("H/T_ref")
    if show_ylabel:
        ax.set_ylabel("D/T_ref")
    masked_values = {value[2] for value in counts.values()}
    nprime_values = {value[3] for value in counts.values()}
    if len(masked_values) == 1 and len(nprime_values) == 1:
        subtitle = f"n'={nprime_values.pop()}, M={masked_values.pop()}"
    else:
        subtitle = ""
    if title:
        ax.set_title(f"{title}\n{subtitle}", fontsize=7.2, linespacing=1.05, pad=3)
    elif subtitle:
        ax.set_title(subtitle, fontsize=5.4, pad=2)
    ax.tick_params(length=0)
    if not show_xticklabels:
        ax.tick_params(labelbottom=False)
    if not show_yticklabels:
        ax.tick_params(labelleft=False)
    ax.set_xticks(np.arange(-0.5, len(H_VALUES), 1), minor=True)
    ax.set_yticks(np.arange(-0.5, len(D_VALUES), 1), minor=True)
    ax.grid(which="minor", color="white", linewidth=0.9)
    ax.tick_params(which="minor", length=0)
    for (i, j), (consumed, prevented, masked, n_prime, total) in counts.items():
        if n_prime == 0:
            ax.add_patch(Rectangle((j - 0.5, i - 0.5), 1, 1, fill=False,
                                   hatch="///", edgecolor="0.55", linewidth=0.5))
            ax.text(j, i, "all masked", ha="center", va="center", fontsize=font_size - 1)
            continue
        value = matrix[i, j]
        color = "black" if value < 0.6 else "white"
        ax.text(j, i, f"C {consumed}\nP {prevented}",
                ha="center", va="center", fontsize=font_size, color=color)
    return image


def build_fig7(rows: list[dict]) -> None:
    fig, axes = plt.subplots(1, 4, figsize=(7.0, 2.45))
    for index, (fault, placement, title) in enumerate(FAULTS):
        image = draw_escape_panel(
            axes[index], rows, 8, "HOST", fault, placement, title,
            show_xlabel=True, show_ylabel=index == 0, font_size=6.2)
    fig.subplots_adjust(left=0.07, right=0.89, bottom=0.24, top=0.80, wspace=0.30)
    cbar = fig.colorbar(image, ax=axes.ravel().tolist(), shrink=0.82, pad=0.025)
    cbar.set_label("corrupt-consumed fraction (C/n')", fontsize=7.5)
    fig.text(0.02, 0.02, "Cell: C = corrupt-consumed count, P = prevented count; "
             "n' = 30 − M. F3M and F4S deliver the same bad data; only F3M reports.",
             fontsize=6.8, color="0.25")
    save_figure(fig, "fig07_escape_efficacy", [FIG_DATA / "fig7_escape.csv"],
                {"n_tiles": 8, "consumer": "HOST",
                 "fault_configs": [f"{fault}/{placement}" for fault, placement, _ in FAULTS]},
                1080,
                "C/P counts over 30 seeds; color = CORRUPT_CONSUMED/(n−MASKED); m=MASKED")


def build_figs2(rows: list[dict]) -> None:
    fig, axes = plt.subplots(4, 4, figsize=(7.0, 8.7))
    image = None
    for row_index, (n_tiles, consumer) in enumerate(
            [(1, "HOST"), (1, "PIM"), (8, "HOST"), (8, "PIM")]):
        for col_index, (fault, placement, title) in enumerate(FAULTS):
            axis = axes[row_index, col_index]
            image = draw_escape_panel(
                axis, rows, n_tiles, consumer, fault, placement,
                f"{title}" if row_index == 0 else "",
                show_xlabel=row_index == 3, show_ylabel=False,
                font_size=5.0, show_xticklabels=row_index == 3,
                show_yticklabels=col_index == 0)
            if col_index == 0:
                axis.set_ylabel(f"n={n_tiles}\n{consumer}", fontsize=6.8)
    fig.text(0.018, 0.50, "D/T_ref", rotation=90, va="center", fontsize=8.5)
    fig.subplots_adjust(left=0.11, right=0.90, bottom=0.07, top=0.94,
                        wspace=0.22, hspace=0.30)
    cbar = fig.colorbar(image, ax=axes.ravel().tolist(), shrink=0.80, pad=0.025)
    cbar.set_label("corrupt-consumed fraction (C/n')", fontsize=7.2)
    fig.text(0.02, 0.01, "Rows: n_tiles × consumer; columns: fault configuration. "
             "Cell: C/P counts; n' = 30 − M.", fontsize=7.0, color="0.25")
    save_figure(fig, "figS2_escape_by_config", [FIG_DATA / "fig7_escape.csv"],
                {"n_tiles": [1, 8], "consumer": ["HOST", "PIM"],
                 "fault_configs": [f"{fault}/{placement}" for fault, placement, _ in FAULTS]},
                4320,
                "C/P counts over 30 seeds per cell; color = CORRUPT_CONSUMED/(n−MASKED)")


def build_fig8() -> None:
    pairs = read_csv(FIG_DATA / "fig8_pairs.csv")
    index = {}
    for row in pairs:
        index[(row["granularity"], int(row["scrub"]), row["scenario_i"],
               row["scenario_j"])] = row
    fig, axes = plt.subplots(2, 3, figsize=(7.0, 5.4))
    image = None
    for row_index, scrub in enumerate((0, 1)):
        for col_index, granularity in enumerate(("bank", "page", "codeword")):
            axis = axes[row_index, col_index]
            matrix = np.full((len(SCENARIOS), len(SCENARIOS)), np.nan)
            counts = {}
            for i, first in enumerate(SCENARIOS):
                for j, second in enumerate(SCENARIOS):
                    valid_first = not (scrub == 0 and first == "S6_SAME_BIT_RECUR")
                    valid_second = not (scrub == 0 and second == "S6_SAME_BIT_RECUR")
                    if i == j:
                        if valid_first:
                            axis.add_patch(Rectangle(
                                (j - 0.5, i - 0.5), 1, 1, facecolor="0.94",
                                edgecolor="none"))
                        else:
                            axis.add_patch(Rectangle(
                                (j - 0.5, i - 0.5), 1, 1, fill=False,
                                hatch="///", edgecolor="0.60", linewidth=0.45))
                        continue
                    if not valid_first or not valid_second:
                        axis.add_patch(Rectangle(
                            (j - 0.5, i - 0.5), 1, 1, fill=False,
                            hatch="///", edgecolor="0.60", linewidth=0.45))
                        continue
                    if i < j:
                        pair = index.get((granularity, scrub, first, second))
                    else:
                        pair = index.get((granularity, scrub, second, first))
                    if pair is None or int(pair["n_seeds"]) == 0:
                        axis.add_patch(Rectangle(
                            (j - 0.5, i - 0.5), 1, 1, fill=False,
                            hatch="///", edgecolor="0.60", linewidth=0.45))
                        continue
                    n_seeds = int(pair["n_seeds"])
                    equal = int(pair["indistinguishable_seeds"])
                    matrix[i, j] = equal / n_seeds
                    if equal > 0:
                        counts[(i, j)] = (equal, n_seeds)
            image = axis.imshow(matrix, cmap="Blues", vmin=0, vmax=1, aspect="auto")
            image.cmap.set_bad("white")
            axis.set_xticks(range(len(SCENARIOS)))
            axis.set_yticks(range(len(SCENARIOS)))
            axis.set_xticklabels(SCENARIO_LABELS, fontsize=7)
            axis.set_yticklabels(SCENARIO_LABELS, fontsize=7)
            for label in axis.get_xticklabels() + axis.get_yticklabels():
                if label.get_text().startswith("S4"):
                    label.set_fontweight("bold")
            axis.set_xticks(np.arange(-0.5, len(SCENARIOS), 1), minor=True)
            axis.set_yticks(np.arange(-0.5, len(SCENARIOS), 1), minor=True)
            axis.grid(which="minor", color="white", linewidth=1.0)
            axis.tick_params(length=0)
            axis.tick_params(which="minor", length=0)
            if row_index == 0:
                axis.set_title(granularity.capitalize(), fontsize=8.5)
            if col_index == 0:
                axis.set_ylabel(f"scrub={scrub}\nscenario", fontsize=7.5)
            if row_index == 1:
                axis.set_xlabel("scenario", fontsize=7.5)
            for (i, j), (equal, n_seeds) in counts.items():
                axis.text(j, i, f"{equal}/{n_seeds}", ha="center", va="center",
                          fontsize=5.8,
                          color="black" if matrix[i, j] < 0.6 else "white")
    fig.subplots_adjust(left=0.12, right=0.89, bottom=0.14, top=0.93,
                        wspace=0.30, hspace=0.32)
    cbar = fig.colorbar(image, ax=axes.ravel().tolist(), shrink=0.82, pad=0.02)
    cbar.set_label("seed fraction with identical reports", fontsize=7.5)
    fig.text(0.5, 0.015, "S4* = two bits in one codeword (danger); hatched = "
             "S6 not run at scrub=0; diagonal omitted.", ha="center",
             fontsize=7.0, color="0.25")
    save_figure(fig, "fig08_report_ambiguity", [FIG_DATA / "fig8_pairs.csv"],
                {"scenarios": SCENARIOS, "scrub": [0, 1],
                 "granularities": ["bank", "page", "codeword"]},
                330,
                "off-diagonal scenario pairs only; color = identical-report "
                "seeds / 10; zero cells left blank; S6 scrub=0 hatched; diagonal omitted")


def main() -> int:
    global FIG_DATA, FIGURES, MANIFEST
    parser = argparse.ArgumentParser()
    parser.add_argument("--results", default=str(ROOT / "results/supplement"))
    parser.add_argument("--figures", default=str(FIGURES))
    args = parser.parse_args()
    FIG_DATA = Path(args.results).resolve() / "figure_data"
    FIGURES = Path(args.figures).resolve()
    MANIFEST = FIGURES / "plot_manifest_supplement.json"
    FIGURES.mkdir(parents=True, exist_ok=True)
    MANIFEST_DATA.clear()
    MANIFEST_DATA.update({
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "stat_seed": STAT_SEED,
        "bootstrap_resamples": 10000,
        "figures": [],
    })
    fig7 = read_csv(FIG_DATA / "fig7_escape.csv")
    build_fig7(fig7)
    build_figs2(fig7)
    build_fig8()
    MANIFEST.write_text(json.dumps(MANIFEST_DATA, indent=2) + "\n")
    print(f"wrote {len(MANIFEST_DATA['figures'])} supplement figures")
    print("G15_PLOT=PASS")
    return 0


MANIFEST_DATA: dict = {}


if __name__ == "__main__":
    raise SystemExit(main())
