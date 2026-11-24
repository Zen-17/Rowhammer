"""Human gate file.  M1 needs G1=GO, and the same rule applies through M4."""
from __future__ import annotations

import json
from pathlib import Path

from .paths import CONFIGS

GATE_FOR_MILESTONE = {"M0": None, "M1": "G1", "M2": "G2", "M3": "G3", "M4": "G4"}


class GateClosed(SystemExit):
    """Raised as ``SystemExit`` so a milestone entry point stops the process."""


def gate_path(path: Path | None = None) -> Path:
    return path or (CONFIGS / "gate_state.json")


def load_gate(path: Path | None = None) -> dict:
    return json.loads(gate_path(path).read_text())


def require(milestone: str, path: Path | None = None) -> None:
    needed = GATE_FOR_MILESTONE[milestone]
    if needed is None:
        return
    state = load_gate(path)
    if state.get(needed) != "GO":
        raise GateClosed(
            f"{milestone} is stopped: {needed}={state.get(needed)!r}. "
            "Waiting for GO / FALLBACK / REVISE."
        )
