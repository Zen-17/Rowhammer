#!/usr/bin/env python3
"""Build the v2.1 calibration from v2 restore/quiesce and L3 no-fault kernel timings."""
from __future__ import annotations

import csv
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
V2 = ROOT / "configs/phase2/calibration.json"
OUT = ROOT / "configs/phase2/v21/calibration.json"
RUNS = ROOT / "runs/phase2/v21/l3"


def fit(points: list[tuple[int, int]]) -> dict:
    xs = [float(x) for x, _ in points]
    ys = [float(y) for _, y in points]
    n = len(xs)
    mean_x = sum(xs) / n
    mean_y = sum(ys) / n
    denom = sum((x - mean_x) ** 2 for x in xs)
    slope = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / denom
    intercept = mean_y - slope * mean_x
    residuals = [y - (intercept + slope * x) for x, y in zip(xs, ys)]
    relative = [abs(r) / (intercept + slope * x) * 100.0 for r, x in zip(residuals, xs)]
    return {
        "c_tile": slope,
        "c_fix": intercept,
        "points": [
            {"n_tiles": int(x), "cycles": int(y), "residual": r}
            for (x, y), r in zip(points, residuals)
        ],
        "relative_residual_percent": relative,
        "max_relative_residual_percent": max(relative),
        "nonlinear": max(relative) > 5.0,
    }


def timing_points(workload: str, kernel_type: str) -> list[tuple[int, int]]:
    points = []
    for n_tiles in (8, 16, 32):
        path = RUNS / f"e2e0_{workload.lower()}_n{n_tiles}_tiles_accon_obson/kernel_timing.csv"
        for row in csv.DictReader(path.open(newline="")):
            if row["kernel_type"] == kernel_type:
                points.append((n_tiles, int(row["t_end"]) - int(row["t_start"])))
    return sorted(points)


def main() -> int:
    payload = json.loads(V2.read_text())
    payload["generated_by"] = "scripts/phase2/v21_calibrate.py"
    payload["schema_version"] = 2
    w1_add = fit(timing_points("W1", "ADD"))
    w2_add = fit(timing_points("W2", "ADD"))
    w2_mul = fit(timing_points("W2", "MUL"))
    w2_relu = fit(timing_points("W2", "RELU"))
    payload["kernel_fits"] = {
        "W1.ADD": {"points": w1_add["points"],
                   "c_fix": w1_add["c_fix"], "c_tile": w1_add["c_tile"],
                   "max_relative_residual_percent": w1_add["max_relative_residual_percent"],
                   "nonlinear": w1_add["nonlinear"]},
        "W2.ADD": {"points": w2_add["points"],
                   "c_fix": w2_add["c_fix"], "c_tile": w2_add["c_tile"],
                   "max_relative_residual_percent": w2_add["max_relative_residual_percent"],
                   "nonlinear": w2_add["nonlinear"]},
        "W2.MUL": {"points": w2_mul["points"],
                   "c_fix": w2_mul["c_fix"], "c_tile": w2_mul["c_tile"],
                   "max_relative_residual_percent": w2_mul["max_relative_residual_percent"],
                   "nonlinear": w2_mul["nonlinear"]},
        "W2.RELU": {"points": w2_relu["points"],
                    "c_fix": w2_relu["c_fix"], "c_tile": w2_relu["c_tile"],
                    "max_relative_residual_percent": w2_relu["max_relative_residual_percent"],
                    "nonlinear": w2_relu["nonlinear"]},
    }
    payload["kernel_costs"] = {
        "W1": [[w1_add["c_fix"], w1_add["c_tile"]]],
        "W2": [[w2_add["c_fix"], w2_add["c_tile"]],
               [w2_mul["c_fix"], w2_mul["c_tile"]],
               [w2_relu["c_fix"], w2_relu["c_tile"]]],
        "W3_K4": [[w1_add["c_fix"], w1_add["c_tile"]]],
        "W3_K16": [[w1_add["c_fix"], w1_add["c_tile"]]],
        "W4_F1": [[w2_add["c_fix"], w2_add["c_tile"]],
                  [w2_mul["c_fix"], w2_mul["c_tile"]],
                  [w2_relu["c_fix"], w2_relu["c_tile"]]],
        "W4_F4": [[w2_add["c_fix"], w2_add["c_tile"]],
                  [w2_mul["c_fix"], w2_mul["c_tile"]],
                  [w2_relu["c_fix"], w2_relu["c_tile"]]],
        "W4_F16": [[w2_add["c_fix"], w2_add["c_tile"]],
                   [w2_mul["c_fix"], w2_mul["c_tile"]],
                   [w2_relu["c_fix"], w2_relu["c_tile"]]],
    }
    payload["assumptions"]["MUL_RELU_tile_cost"] = (
        "v2.1 fits ADD/MUL/RELU separately from L3 no-fault kernel timings; "
        "the v2 ASSUMED ADD-cost reuse is no longer used.")
    payload["calibration_split"] = {
        "fit": "L3 E2E-0 no-fault W1/W2 n_tiles={8,16,32}",
        "validation": "all L3 W1/W2 fault recovery cases",
    }
    OUT.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(f"CALIBRATION={OUT.relative_to(ROOT)}")
    for name, fit_result in (("W1.ADD", w1_add), ("W2.ADD", w2_add),
                             ("W2.MUL", w2_mul), ("W2.RELU", w2_relu)):
        print(f"{name}: c_fix={fit_result['c_fix']:.3f} "
              f"c_tile={fit_result['c_tile']:.3f} "
              f"max_rel={fit_result['max_relative_residual_percent']:.2f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
