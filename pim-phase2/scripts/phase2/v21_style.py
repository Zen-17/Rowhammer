"""v2.1 figure style: camera-ready architecture-conference aesthetics."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import List

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams.update({
    "font.family": "DejaVu Serif",
    "font.size": 8,
    "axes.labelsize": 8,
    "axes.titlesize": 8,
    "xtick.labelsize": 7,
    "ytick.labelsize": 7,
    "legend.fontsize": 7,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.linewidth": 0.6,
    "xtick.major.width": 0.6,
    "ytick.major.width": 0.6,
    "xtick.major.size": 2.5,
    "ytick.major.size": 2.5,
    "lines.linewidth": 1.1,
    "lines.markersize": 3.2,
    "savefig.dpi": 600,
    "figure.dpi": 150,
    "axes.grid": False,
})

SINGLE = (3.45, 2.35)
DOUBLE = (7.16, 2.55)
WIDE = (7.16, 4.1)
COLORS = {
    "B0": "#5A5A5A",
    "B1": "#0072B2",
    "B2": "#009E73",
    "Oracle": "#000000",
    "W1": "#0072B2",
    "W2": "#D55E00",
    "W3": "#009E73",
    "W3_K4": "#009E73",
    "W3_K16": "#CC79A7",
    "W4_F1": "#56B4E9",
    "W4_F4": "#E69F00",
    "W4_F16": "#CC79A7",
    "GLOBAL_HOLD": "#0072B2",
    "DEP_RELEASE": "#D55E00",
    "L3": "#000000",
}
MARKERS = {"B0": "s", "B1": "o", "B2": "^", "Oracle": "_",
           "W1": "o", "W2": "s", "W3": "^", "W3_K4": "^", "W3_K16": "v",
           "W4_F1": "o", "W4_F4": "s", "W4_F16": "^"}
STATUS_COLORS = {
    "SOURCE_REPAIRED_ONLY": "#56B4E9",
    "SELECTIVE_RECOVERED": "#009E73",
    "PARTIAL_WITH_ESCAPE": "#E69F00",
    "WHOLE_JOB_FALLBACK": "#5A5A5A",
    "OUTSIDE_WINDOW_NO_TRUSTED_INPUT": "#D55E00",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def save_figure(fig, name: str, figures: Path, manifest: List[dict],
                inputs: List[Path], filters: dict, sample_count: int,
                aggregation: str, caption: str) -> None:
    figures.mkdir(parents=True, exist_ok=True)
    outputs = []
    for suffix in ("pdf", "svg", "png"):
        path = figures / f"{name}.{suffix}"
        fig.savefig(path, bbox_inches="tight", facecolor="white")
        outputs.append({"path": str(path), "sha256": sha256(path)})
    plt.close(fig)
    manifest.append({
        "figure": name,
        "inputs": [{"path": str(path), "sha256": sha256(path)} for path in inputs],
        "filters": filters,
        "sample_count": sample_count,
        "aggregation": aggregation,
        "outputs": outputs,
        "caption": caption,
    })


def panel_label(axis, text: str) -> None:
    axis.text(0.01, 1.01, text, transform=axis.transAxes, fontsize=8,
              fontweight="bold", va="bottom", ha="left")
