#!/usr/bin/env python3
"""F6: compare v2.1 L2 predictions with all L3 W1/W2 recovery cases."""
from __future__ import annotations

import csv
import json
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from phase2.evaluate import CaseSpec, evaluate_case
from phase2.timing import CostModel

RUNS = ROOT / "runs/phase2/v21/l3"
OUT = ROOT / "results/phase2/v21/figure_data/p2f6_validation.csv"
SUMMARY = ROOT / "results/phase2/v21/l3/p2f6_summary.json"


def read_params(path: Path) -> dict:
    values = {}
    for line in path.read_text().splitlines():
        key, _, value = line.partition("=")
        if key and not key.startswith("#"):
            values[key.strip()] = value.strip()
    return values


def metric_row(workload: str, run_id: str, metric: str, l2: float, l3: float,
               split: str) -> dict:
    error = abs(l2 - l3) / l3 if l3 else ""
    return {"workload": workload, "run_id": run_id, "metric": metric,
            "l2_value": l2, "l3_value": l3, "relative_error": error,
            "split": split, "source": "L2/L3"}


def main() -> int:
    costs = CostModel.from_calibration(ROOT / "configs/phase2/v21/calibration.json")
    rows: list[dict] = []
    for run_dir in sorted(RUNS.iterdir()):
        if not run_dir.is_dir():
            continue
        metrics_path = run_dir / "metrics.json"
        inputs_path = run_dir / "decision_inputs.json"
        params_path = run_dir / "params.ini"
        if not (metrics_path.exists() and inputs_path.exists() and params_path.exists()):
            continue
        metrics = json.loads(metrics_path.read_text())
        if metrics.get("fault_kind") not in ("F1", "F2"):
            continue
        if str(metrics.get("negative_control", "NONE")) != "NONE":
            continue
        workload = str(metrics.get("workload"))
        if workload not in ("W1", "W2"):
            continue
        params = read_params(params_path)
        inputs = json.loads(inputs_path.read_text())
        policy = str(metrics.get("policy"))
        if policy not in ("B0", "B1", "B2"):
            continue
        case = CaseSpec(
            kind="main", workload=workload, n_tiles=int(metrics["n_tiles"]),
            seed=int(metrics["seed"]), granularity=str(metrics["granularity"]),
            error_class="CE" if metrics["fault_kind"] == "F1" else "UE",
            fault_operand=int(metrics["fault_operand"]),
            fault_tile=int(metrics["fault_tile"]), policy=policy,
            transport_delay=int(metrics["transport_delay"]),
            engine_delta=int(inputs.get("delta_cycles", metrics["transport_delay"] + 65)),
            window_mode=str(inputs.get("window_mode", "AUTO")),
            baseline_end_to_end=float(metrics.get("baseline_end_to_end_cycles", 0) or 0),
            max_log_entries=int(params.get("max_log_entries", "1000000")))
        predicted = evaluate_case(case, costs)
        l3_recovery = float(metrics.get("recovery_cycles_excl_resume", 0) or 0)
        l3_overhead = float(metrics.get("fault_overhead_cycles", 0) or 0)
        l2_recovery = float(predicted.get("recovery_cycles_excl_resume", 0) or 0)
        l2_overhead = float(predicted.get("fault_overhead_cycles", 0) or 0)
        rows.append(metric_row(workload, run_dir.name, "recovery_cycles_excl_resume",
                               l2_recovery, l3_recovery, "validation"))
        rows.append(metric_row(workload, run_dir.name, "fault_overhead_cycles",
                               l2_overhead, l3_overhead, "validation"))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()) if rows else [])
        writer.writeheader()
        writer.writerows(rows)
    summary = {"rows": len(rows), "by_workload_metric": {}}
    for workload in ("W1", "W2"):
        for metric in ("recovery_cycles_excl_resume", "fault_overhead_cycles"):
            subset = [row for row in rows
                      if row["workload"] == workload and row["metric"] == metric
                      and row["relative_error"] != ""]
            if not subset:
                continue
            mape = statistics.mean(float(row["relative_error"]) for row in subset) * 100.0
            summary["by_workload_metric"][f"{workload}/{metric}"] = {
                "n": len(subset), "mape_percent": mape}
    SUMMARY.write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
