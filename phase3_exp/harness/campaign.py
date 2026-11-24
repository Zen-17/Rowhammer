"""Per-seed run layout.  Successful logs are immutable.  Scales never share a file."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Callable, Optional

from .gate import require
from .paths import RESULTS, RUNS

SCALES = ("scale_real", "scale_1_16")


def run_dir(milestone: str, experiment: str, scale: str, grid_id: str) -> Path:
    if scale not in SCALES:
        raise ValueError(f"unknown scale {scale}; real and 1/16 stay in separate trees")
    safe = grid_id.replace("/", "_")
    return RUNS / milestone / experiment / scale / safe


def result_path(name: str, scale: str) -> Path:
    if scale not in SCALES:
        raise ValueError(f"unknown scale {scale}")
    return RESULTS / scale / name


def write_run(
    milestone: str,
    experiment: str,
    scale: str,
    grid_id: str,
    seed: int,
    payload: dict,
    producer: Optional[Callable[[], dict]] = None,
) -> Path:
    require(milestone)
    if "STATUS" not in payload:
        raise ValueError("every run payload needs STATUS")
    directory = run_dir(milestone, experiment, scale, grid_id)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{int(seed)}.json"
    if path.exists():
        previous = json.loads(path.read_text())
        if previous.get("STATUS") == "SUCCESS":
            raise FileExistsError(f"refusing to overwrite a successful log: {path}")
    body = dict(payload)
    body["seed"] = int(seed)
    body["scale"] = scale
    body["grid_id"] = grid_id
    if producer is not None:
        body.update(producer())
    path.write_text(json.dumps(body, indent=2, sort_keys=True) + "\n")
    return path


def load_scale(milestone: str, experiment: str, scale: str) -> list:
    root = RUNS / milestone / experiment / scale
    if not root.exists():
        return []
    rows = []
    for path in sorted(root.rglob("*.json")):
        rows.append(json.loads(path.read_text()))
    return rows
