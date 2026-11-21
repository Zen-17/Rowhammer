#!/usr/bin/env python3
"""Calibrate the L2 cost model from frozen Phase 1 measurements.

Inputs (all relative to the repository root):

  * results/g7/summary.csv              -- positive G7 recovery rows
  * results/figure_data/fig4_quiescence.csv -- measured quiesce cycles

The calibration deliberately uses only medians per ``n_tiles``.  Each fitted
parameter records its source, the exact column, the sample count and the
residual at every fitted point.
"""
from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

import pandas as pd

ROOT = Path(__file__).resolve().parents[2]
P1_ROOT = ROOT.parent / "pim-phase1"
G7_SUMMARY = P1_ROOT / "results/g7/summary.csv"
QUIESCE = P1_ROOT / "results/figure_data/fig4_quiescence.csv"
FIG6_COST = P1_ROOT / "results/figure_data/fig6_cost.csv"
OUT = ROOT / "configs/phase2/calibration.json"

EXPECTED = {
    "c_tile": 475.0,
    "c_fix": 329.0,
    "c_restore_per_byte": 0.0648,
    "c_restore_fix": 26.0,
    "c_quiesce": 41.0,
}


def linear_fit(xs: list[float], ys: list[float]) -> tuple[float, float, list[float]]:
    """Ordinary least squares y = intercept + slope * x; returns (slope, intercept, residuals)."""
    if len(xs) != len(ys) or len(xs) < 2:
        raise ValueError("linear_fit needs at least two paired points")
    n = float(len(xs))
    mean_x = sum(xs) / n
    mean_y = sum(ys) / n
    denom = sum((x - mean_x) ** 2 for x in xs)
    if denom == 0.0:
        raise ValueError("degenerate x values")
    slope = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / denom
    intercept = mean_y - slope * mean_x
    residuals = [y - (intercept + slope * x) for x, y in zip(xs, ys)]
    return slope, intercept, residuals


def provenance(path: Path, column: str, rows: int, extra: dict | None = None) -> dict:
    try:
        file_text = str(path.relative_to(ROOT.parent))
    except ValueError:
        file_text = str(path)
    payload = {
        "file": file_text,
        "sha256": __import__("hashlib").sha256(path.read_bytes()).hexdigest(),
        "column": column,
        "rows": rows,
    }
    if extra:
        payload.update(extra)
    return payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(OUT))
    args = parser.parse_args()

    g7 = pd.read_csv(G7_SUMMARY)
    positives = g7[g7["kind"] == "positive"].copy()
    if positives.empty:
        raise SystemExit("no positive G7 rows to calibrate from")
    by_tiles = (positives.groupby("n_tiles")[["reexec_cycles", "restore_cycles", "restore_bytes"]]
                .median()
                .sort_index())
    tiles = [float(v) for v in by_tiles.index]
    reexec = [float(v) for v in by_tiles["reexec_cycles"]]
    restore = [float(v) for v in by_tiles["restore_cycles"]]
    restore_bytes = [float(v) for v in by_tiles["restore_bytes"]]

    c_tile, c_fix, reexec_residuals = linear_fit(tiles, reexec)
    c_restore_per_byte, c_restore_fix, restore_residuals = linear_fit(restore_bytes, restore)

    quiesce = pd.read_csv(QUIESCE)
    actual = quiesce[quiesce["scenario"] == "ACTUAL_CE"]
    if actual.empty:
        actual = quiesce
    c_quiesce = float(statistics.median(actual["quiesce_cycles"].astype(float).tolist()))

    fig6 = pd.read_csv(FIG6_COST)
    ext_host = fig6[(fig6["policy"] == "EXT_OFF") & (fig6["consumer"] == "HOST")]
    consumer_fit = None
    if not ext_host.empty:
        per_tiles = (ext_host.groupby("n_tiles")[["end_to_end_cycles", "producer_cycles"]]
                     .median().sort_index())
        host_extra = [float(row.end_to_end_cycles - row.producer_cycles)
                      for row in per_tiles.itertuples()]
        c_host_per_tile, c_host_fix, host_residuals = linear_fit(
            [float(v) for v in per_tiles.index], host_extra)
        consumer_fit = {
            "model": "c_host_fix + n_tiles * c_host_per_tile",
            "parameters": {"c_host_fix": c_host_fix,
                           "c_host_per_tile": c_host_per_tile},
            "points": [
                {"n_tiles": int(n), "median_host_extra_cycles": y, "residual": r}
                for n, y, r in zip(per_tiles.index, host_extra, host_residuals)
            ],
            "provenance": provenance(
                FIG6_COST, "end_to_end_cycles - producer_cycles", int(len(ext_host)),
                {"filter": "policy==EXT_OFF, consumer==HOST",
                 "aggregation": "median per n_tiles"}),
            "assumed_extrapolation": "n_tiles 32/128 are extrapolated",
        }

    parameters = {
        "c_tile": c_tile,
        "c_fix": c_fix,
        "c_restore_per_byte": c_restore_per_byte,
        "c_restore_fix": c_restore_fix,
        "c_quiesce": c_quiesce,
    }
    checks = {}
    for key, expected in EXPECTED.items():
        delta = parameters[key] - expected
        checks[key] = {
            "expected": expected,
            "observed": parameters[key],
            "delta": delta,
            "pass": abs(delta) <= 1.0,
        }

    payload = {
        "schema_version": 1,
        "generated_by": "scripts/phase2/calibrate.py",
        "parameters": parameters,
        "reference_check": checks,
        "reference_check_pass": all(item["pass"] for item in checks.values()),
        "fits": {
            "reexec_cycles": {
                "model": "c_fix + n_tiles * c_tile",
                "points": [
                    {"n_tiles": int(n), "median_reexec_cycles": y, "residual": r}
                    for n, y, r in zip(tiles, reexec, reexec_residuals)
                ],
                "provenance": provenance(
                    G7_SUMMARY, "reexec_cycles", int(len(positives)),
                    {"filter": "kind==positive", "aggregation": "median per n_tiles"}),
                "assumed": False,
            },
            "restore_cycles": {
                "model": "c_restore_fix + restore_bytes * c_restore_per_byte",
                "points": [
                    {"restore_bytes": int(b), "median_restore_cycles": y, "residual": r}
                    for b, y, r in zip(restore_bytes, restore, restore_residuals)
                ],
                "provenance": provenance(
                    G7_SUMMARY, "restore_cycles", int(len(positives)),
                    {"filter": "kind==positive", "aggregation": "median per n_tiles"}),
                "assumed": False,
            },
            "quiesce_cycles": {
                "model": "median",
                "points": [
                    {"scenario": row.scenario, "handler_delay": int(row.handler_delay),
                     "quiesce_cycles": int(row.quiesce_cycles)}
                    for row in actual.itertuples()
                ],
                "provenance": provenance(
                    QUIESCE, "quiesce_cycles", int(len(actual)),
                    {"filter": "scenario==ACTUAL_CE" if not actual.equals(quiesce) else "all"}),
                "assumed": False,
            },
        },
        "assumptions": {
            "MUL_RELU_tile_cost": "MUL/RELU reuse the ADD per-tile cost (ASSUMED until L3)",
            "fixed_overhead_split": "half of c_fix is charged before the first tile (ASSUMED)",
        },
        "consumer_model": consumer_fit,
    }

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(f"CALIBRATION={out.relative_to(ROOT)}")
    print(f"c_tile={c_tile:.4f} c_fix={c_fix:.4f} "
          f"c_restore_per_byte={c_restore_per_byte:.6f} c_restore_fix={c_restore_fix:.4f} "
          f"c_quiesce={c_quiesce:.1f}")
    if consumer_fit:
        print(f"consumer: c_host_fix={consumer_fit['parameters']['c_host_fix']:.4f} "
              f"c_host_per_tile={consumer_fit['parameters']['c_host_per_tile']:.4f}")
    print(f"REFERENCE_CHECK={'PASS' if payload['reference_check_pass'] else 'FAIL'}")
    return 0 if payload["reference_check_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
