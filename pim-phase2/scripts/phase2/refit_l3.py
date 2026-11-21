#!/usr/bin/env python3
"""Stage 6: refit L3 cost parameters on W1 and validate on W2 (P2-F6)."""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from phase2.evaluate import CaseSpec, evaluate_case  # noqa: E402
from phase2.timing import CostModel  # noqa: E402

RUNS = ROOT / "runs/phase2/l3"
CALIBRATION = ROOT / "configs/phase2/calibration.json"
OUT_CSV = ROOT / "results/phase2/figure_data/p2f6_validation.csv"
OUT_JSON = ROOT / "configs/phase2/l3_refit.json"


def read_json(path: Path) -> dict:
    return json.loads(path.read_text())


def read_params(path: Path) -> dict:
    params = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        key, _, value = line.partition("=")
        params[key.strip()] = value.strip()
    return params


def fit_two_points(x8: float, y8: float, x32: float, y32: float) -> tuple[float, float]:
    slope = (y32 - y8) / (x32 - x8)
    intercept = y8 - slope * x8
    return slope, intercept


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", default=str(RUNS))
    parser.add_argument("--calibration", default=str(CALIBRATION))
    args = parser.parse_args()
    run_root = Path(args.runs)
    base = CostModel.from_calibration(Path(args.calibration))
    w1_n8 = read_json(run_root / "e2e0_w1_n8_tiles_accon_obson/metrics.json")
    w1_n32 = read_json(run_root / "e2e0_w1_n32_tiles_accon_obson/metrics.json")
    t8 = float(w1_n8["producer_cycles"])
    t32 = float(w1_n32["producer_cycles"])
    c_tile, c_fix = fit_two_points(8.0, t8, 32.0, t32)
    refit = CostModel(c_tile=c_tile, c_fix=c_fix,
                      c_restore_per_byte=base.c_restore_per_byte,
                      c_restore_fix=base.c_restore_fix,
                      c_quiesce=base.c_quiesce,
                      c_host_fix=base.c_host_fix,
                      c_host_per_tile=base.c_host_per_tile)
    (ROOT / "configs/phase2/l3_refit.json").write_text(json.dumps({
        "schema_version": 1,
        "source": "Phase 2 L3 E2E-0 no-fault W1 runs",
        "fit_points": [
            {"n_tiles": 8, "producer_cycles": t8},
            {"n_tiles": 32, "producer_cycles": t32},
        ],
        "parameters": {"c_tile": c_tile, "c_fix": c_fix},
        "validation": "W2 sweep runs, never used in the fit",
        "residuals": {
            "n_tiles_8": t8 - (c_fix + 8 * c_tile),
            "n_tiles_32": t32 - (c_fix + 32 * c_tile),
        },
    }, indent=2, sort_keys=True) + "\n")

    rows = []
    for run_dir in sorted(run_root.glob("sweep_*")):
        params_path = run_dir / "params.ini"
        metrics_path = run_dir / "metrics.json"
        if not params_path.exists() or not metrics_path.exists():
            continue
        params = read_params(params_path)
        if params.get("policy") != "B1":
            continue
        if params.get("granularity") != "page":
            continue
        metrics = read_json(metrics_path)
        if metrics.get("recovery_cycles_incl_quiesce") is None:
            continue
        case = CaseSpec(
            kind="main",
            workload=params["workload"],
            n_tiles=int(params["n_tiles"]),
            seed=int(params["seed"]),
            granularity=params["granularity"],
            error_class="CE" if params["fault_kind"] == "F1" else "UE",
            fault_operand=int(params["fault_operand"]),
            fault_tile=int(params["fault_tile"]),
            policy=params["policy"],
            transport_delay=int(params["transport_delay"]),
            engine_delta=int(metrics["delta_cycles"]),
            max_log_entries=int(params["max_log_entries"]),
        )
        prediction = evaluate_case(case, refit)
        measured = float(metrics["recovery_cycles_incl_quiesce"])
        predicted = float(prediction["recovery_cycles"])
        split = "fit" if params["workload"] == "W1" else "validation"
        rows.append({
            "figure": "P2-F6",
            "workload": params["workload"],
            "n_tiles": int(params["n_tiles"]),
            "granularity": params["granularity"],
            "error_class": case.error_class,
            "policy": params["policy"],
            "seed": int(params["seed"]),
            "fault_tile": int(params["fault_tile"]),
            "fault_operand": int(params["fault_operand"]),
            "l2_recovery_cycles": round(predicted, 3),
            "l3_recovery_cycles": round(measured, 3),
            "relative_error": round((predicted - measured) / measured, 6),
            "split": split,
            "source": "L2/L3",
        })
    if not rows:
        raise SystemExit("no L3 sweep runs for P2-F6")
    out = Path(OUT_CSV)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    validation = [row for row in rows if row["split"] == "validation"]
    fit = [row for row in rows if row["split"] == "fit"]
    mape = sum(abs(row["relative_error"]) for row in validation) / len(validation) * 100.0
    fit_mape = sum(abs(row["relative_error"]) for row in fit) / len(fit) * 100.0
    summary = {
        "fit_rows": len(fit),
        "validation_rows": len(validation),
        "fit_mape_percent": fit_mape,
        "validation_mape_percent": mape,
        "fit_parameters": {"c_tile": c_tile, "c_fix": c_fix},
        "note": "W2 validation never participates in the fit; MUL/RELU reuse ADD cost.",
    }
    (ROOT / "results/phase2/l3/p2f6_summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(f"FIT c_tile={c_tile:.4f} c_fix={c_fix:.4f}")
    print(f"P2F6_ROWS={len(rows)} FIT={len(fit)} VALIDATION={len(validation)}")
    print(f"P2F6_FIT_MAPE={fit_mape:.2f}% P2F6_VALIDATION_MAPE={mape:.2f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
