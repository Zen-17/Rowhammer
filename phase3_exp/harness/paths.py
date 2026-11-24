"""Repository paths for the Phase 3 framework."""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
P1 = REPO / "pim-phase1"
P2 = REPO / "pim-phase2"
CONFIGS = ROOT / "configs"
WORKLOADS = CONFIGS / "workloads"
RUNS = ROOT / "runs"
RESULTS = ROOT / "results"
FIGURES = ROOT / "figures"
REPORTS = ROOT / "reports"
MC = ROOT / "mc"


def load_json(path: Path) -> dict:
    return json.loads(path.read_text())


def ensure_dirs() -> None:
    for path in (RUNS, RESULTS, FIGURES, REPORTS, MC):
        path.mkdir(parents=True, exist_ok=True)
