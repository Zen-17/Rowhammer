"""Camera-ready style.  Every Phase 3 figure must import this module and no other.

The geometry matches the Phase 2 v2.1 architecture-conference sheet: DejaVu Serif,
a 3.45 in column, a 7.16 in text width, Okabe-Ito colors, and embedded TrueType.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Dict, Iterable, List, Sequence

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

SCHEME_ORDER = ["NONE", "REACT", "PARA", "FIX", "ADA", "OURS"]
SCHEME_COLORS = {
    "NONE": "#5A5A5A",
    "REACT": "#E69F00",
    "PARA": "#56B4E9",
    "FIX": "#0072B2",
    "ADA": "#009E73",
    "OURS": "#D55E00",
}
ABLATION_HATCH = "//"
WORKLOAD_MARKERS = {"W1": "o", "W2": "s", "W4": "^", "GEMV": "D", "DEC": "v"}
WORKLOAD_ORDER = ["W1", "W2", "W4", "GEMV", "DEC"]
F_TH = 0.0625
G1_REFERENCE = 0.02
HEATMAP_CMAP = "cividis"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def panel_label(axis, text: str) -> None:
    axis.text(0.01, 1.02, text, transform=axis.transAxes, fontsize=8,
              fontweight="bold", va="bottom", ha="left")


def save_figure(fig, name: str, figures: Path, manifest: List[dict],
                inputs: Sequence[Path], filters: dict, sample_count: int,
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


def write_manifest(figures: Path, manifest: Iterable[dict]) -> None:
    path = figures / "manifest.json"
    path.write_text(json.dumps(list(manifest), indent=2) + "\n")


def scheme_color(name: str) -> str:
    base = name.split("-")[0]
    if name.startswith("OURS"):
        base = "OURS"
    if base not in SCHEME_COLORS:
        raise KeyError(f"no locked color for {name}")
    return SCHEME_COLORS[base]


def reference_lines(axis) -> Dict[str, float]:
    """Threshold guides.  They are annotated on the axes and kept out of the legend."""
    axis.axhline(F_TH, color="#000000", linestyle="--", linewidth=0.7, zorder=0)
    axis.axhline(G1_REFERENCE, color="#666666", linestyle=":", linewidth=0.7, zorder=0)
    axis.text(1.0, F_TH, " f_th = 0.0625", transform=axis.get_yaxis_transform(),
              va="bottom", ha="right", fontsize=6, color="#000000")
    axis.text(1.0, G1_REFERENCE, " G1 = 0.02", transform=axis.get_yaxis_transform(),
              va="top", ha="right", fontsize=6, color="#666666")
    return {"f_th": F_TH, "g1": G1_REFERENCE}
