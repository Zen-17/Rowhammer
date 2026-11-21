"""DIFF: Python engine independently reproduces every exported C++ decision."""
from __future__ import annotations

from pathlib import Path

import pytest

from scripts.phase2.check_differential import check_run

ROOT = Path(__file__).resolve().parents[2]
RUN_ROOT = ROOT / "runs/phase2/l3"


def run_dirs():
    if not RUN_ROOT.exists():
        return []
    return sorted(path.parent for path in RUN_ROOT.glob("*/decision.json"))


@pytest.mark.parametrize("run_dir", run_dirs(), ids=lambda path: path.name)
def test_decision_matches_python_engine(run_dir: Path) -> None:
    ok, detail = check_run(run_dir)
    assert ok, detail


def test_has_runs() -> None:
    assert run_dirs(), "no L3 decisions exported yet"
