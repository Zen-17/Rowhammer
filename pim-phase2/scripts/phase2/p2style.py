"""Shared Phase 2 figure style, statistics helpers and output bookkeeping."""
from __future__ import annotations

import csv
import hashlib
import json
import math
from pathlib import Path
from typing import Iterable, List

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

SINGLE = (3.5, 2.4)
DOUBLE = (7.16, 2.6)
STAT_SEED = 20261115
BOOTSTRAP_N = 10000

POLICY_ORDER = ["B0", "B1", "B2", "B3", "B2R-0", "B2R-1", "B2R-2", "Oracle"]
POLICY_COLORS = {
    "B0": "#666666",
    "B1": "#0072B2",
    "B2": "#009E73",
    "B3": "#D55E00",
    "B2R-0": "#E8C4DA",
    "B2R-1": "#CC79A7",
    "B2R-2": "#8E3B66",
    "Oracle": "#000000",
}
POLICY_HATCH = {
    "B0": "",
    "B1": "",
    "B2": "///",
    "B3": "\\\\\\",
    "B2R-0": "...",
    "B2R-1": "///",
    "B2R-2": "xxx",
    "Oracle": "",
}
POLICY_MARKER = {
    "B0": "s", "B1": "o", "B2": "^", "B3": "D",
    "B2R-0": "v", "B2R-1": "P", "B2R-2": "X", "Oracle": "_",
}
STATUS_COLORS = {
    "SOURCE_REPAIRED_ONLY": "#0072B2",
    "SELECTIVE_RECOVERED": "#009E73",
    "PARTIAL_WITH_ESCAPE": "#E69F00",
    "WHOLE_JOB_FALLBACK": "#666666",
    "OUTSIDE_WINDOW_NO_TRUSTED_INPUT": "#D55E00",
}
DISTANCE_MARKERS = {
    "same_row": "o", "minus1": "v", "plus1": "^",
    "minus2": "<", "plus2": ">", "far": "s",
}


def policy_style(policy: str) -> dict:
    is_oracle = policy == "Oracle"
    return {
        "color": POLICY_COLORS.get(policy, "#999999"),
        "hatch": POLICY_HATCH.get(policy, ""),
        "marker": POLICY_MARKER.get(policy, "o"),
        "linestyle": "--" if is_oracle else "-",
    }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_csv(path: Path) -> List[dict]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def number(value) -> float | None:
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def bootstrap_ci(values: Iterable[float], n_boot: int = BOOTSTRAP_N,
                 seed: int = STAT_SEED) -> tuple[float | None, float | None]:
    clean = np.asarray([v for v in values if v is not None], dtype=float)
    if clean.size < 2:
        return None, None
    rng = np.random.Generator(np.random.PCG64(seed))
    means = np.empty(n_boot)
    for index in range(n_boot):
        means[index] = clean[rng.integers(0, clean.size, clean.size)].mean()
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


def panel_label(axis, text: str) -> None:
    axis.text(0.01, 1.02, text, transform=axis.transAxes, fontsize=9,
              fontweight="bold", va="bottom", ha="left")


def save_figure(fig, name: str, figures: Path, manifest: List[dict],
                inputs: List[Path], filters: dict, sample_count: int,
                aggregation: str, captions: dict) -> None:
    outputs = []
    for suffix in ("pdf", "svg", "png"):
        path = figures / f"{name}.{suffix}"
        fig.savefig(path, bbox_inches="tight", facecolor="white")
        outputs.append({"path": str(path), "sha256": sha256(path)})
    gray_path = figures / f"{name}_gray.png"
    Image.open(figures / f"{name}.png").convert("L").save(gray_path)
    outputs.append({"path": str(gray_path), "sha256": sha256(gray_path)})
    plt.close(fig)
    manifest.append({
        "figure": name,
        "inputs": [{"path": str(path), "sha256": sha256(path)} for path in inputs],
        "columns": sorted({key for path in inputs for key in read_csv(path)[0].keys()})
        if all(path.exists() and read_csv(path) for path in inputs) else [],
        "filters": filters,
        "sample_count": sample_count,
        "aggregation": aggregation,
        "outputs": outputs,
        "caption": captions.get(name, ""),
    })


def write_manifest(path: Path, manifest: List[dict]) -> None:
    path.write_text(json.dumps({"schema_version": 1, "figures": manifest},
                               indent=2, sort_keys=True) + "\n")
