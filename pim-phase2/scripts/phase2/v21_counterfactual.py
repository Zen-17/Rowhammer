#!/usr/bin/env python3
"""Classify F2 recovery counterfactuals from paired L3 runs (Fix-8a)."""
from __future__ import annotations

import csv
import json
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN_METRICS = ROOT / "results/phase2/v21/l3/run_metrics.csv"
OUT = ROOT / "results/phase2/v21/l3/f2_counterfactual.csv"


def truthy(value) -> bool:
    return str(value).strip().lower() in ("1", "true", "yes")


def main() -> int:
    rows = list(csv.DictReader(RUN_METRICS.open(newline="")))
    by_id = {row["run_id"]: row for row in rows}
    results = []
    for row in rows:
        if row["group"] != "SWEEP" or row["fault_kind"] != "F2":
            continue
        if row.get("negative_control") == "NO_REPLAY":
            continue
        norep_id = f"{row['run_id']}_norep"
        norep = by_id.get(norep_id)
        if norep is None:
            continue
        normal_ok = truthy(row["output_bit_exact"])
        norep_ok = truthy(norep["output_bit_exact"])
        if not normal_ok:
            classification = "FAIL"
        elif not norep_ok:
            classification = "RECOVERY_NEEDED_AND_DONE"
        else:
            classification = "MASKED"
        results.append({
            "run_id": row["run_id"],
            "workload": row["workload"],
            "n_tiles": row["n_tiles"],
            "granularity": row["granularity"],
            "policy": row["policy"],
            "fault_tile": row["fault_tile"],
            "seed": row["seed"],
            "normal_output_exact": normal_ok,
            "norep_output_exact": norep_ok,
            "classification": classification,
        })
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(results[0].keys()) if results else [])
        writer.writeheader()
        writer.writerows(results)
    counts = Counter(row["classification"] for row in results)
    payload = {"rows": len(results), "counts": dict(counts),
               "source": str(OUT.relative_to(ROOT))}
    (ROOT / "results/phase2/v21/l3/f2_counterfactual_summary.json").write_text(
        json.dumps(payload, indent=2) + "\n")
    print(f"F2_COUNTERFACTUAL={len(results)} {json.dumps(dict(counts), sort_keys=True)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
