#!/usr/bin/env python3
"""Collect L3 E2E measurements for the P2-F2 diamond overlay."""
from __future__ import annotations

import csv
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNS = ROOT / "runs/phase2/l3"
OUT = ROOT / "results/phase2/figure_data/p2f2_l3.csv"


def read_metrics(run_id: str) -> dict:
    return json.loads((RUNS / run_id / "metrics.json").read_text())


def main() -> int:
    rows = []
    groups = [
        (["e2e1_page_ue_k0", "e2e1_page_ue_k3", "e2e1_page_ue_k7"], "UE", "B1"),
        (["e2e2_b0_k0", "e2e2_b0_k3", "e2e2_b0_k7"], "UE", "B0"),
        (["e2e4_ce_b2_k3"], "CE", "B2"),
    ]
    for run_ids, error_class, policy in groups:
        for index, run_id in enumerate(run_ids):
            metrics = read_metrics(run_id)
            rows.append({
                "figure": "P2-F2",
                "workload": "W1",
                "n_tiles": metrics["n_tiles"],
                "granularity": metrics["granularity"],
                "error_class": error_class,
                "policy": policy,
                "seed": index,
                "fault_tile": metrics["fault_tile"],
                "recovery_cycles": metrics["recovery_cycles"],
                "source": "L3",
            })
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"P2F2_L3_ROWS={len(rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
