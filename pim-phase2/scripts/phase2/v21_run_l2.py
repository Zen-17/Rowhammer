#!/usr/bin/env python3
"""Run the Phase 2 v2.1 L2 matrix and write v21 result files."""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from phase2.evaluate import (  # noqa: E402
    CaseSpec,
    evaluate_case,
    expand_late_cases,
    expand_ledger_cases,
    expand_main_cases,
    expand_radius_cases,
    expand_release_cases,
    expand_status_cases,
    expand_window_control_cases,
)
from phase2.timing import CostModel  # noqa: E402

DEFAULT_CONFIG = ROOT / "configs/phase2/v21/l2_suite.yaml"


def evaluate_one(args) -> dict:
    case_dict, calibration_path = args
    costs = CostModel.from_calibration(Path(calibration_path))
    return evaluate_case(CaseSpec(**case_dict), costs)


def build_cases(config: dict) -> list[CaseSpec]:
    seeds = range(int(config["seeds"]["start"]), int(config["seeds"]["stop"]))
    matrices = config["matrices"]
    cases: list[CaseSpec] = []
    main = matrices["main"]
    cases += expand_main_cases(
        seeds=seeds, workloads=main["workloads"], n_tile_values=main["n_tiles"],
        granularities=main["granularity"], error_classes=main["error_class"],
        policies=main["policies"], delays=main["transport_delay"],
        window_mode=main.get("window_mode", "AUTO"))
    window = matrices["window_control"]
    cases += expand_window_control_cases(
        seeds=seeds, workloads=window["workloads"], n_tiles=window["n_tiles"][0],
        granularities=window["granularity"], policies=window["policies"],
        delays=window["transport_delay"])
    radius = matrices["radius"]
    cases += expand_radius_cases(
        seeds=seeds, distances=radius["distances"], policies=radius["policies"],
        n_tiles=radius["n_tiles"][0], delay=radius["transport_delay"][0])
    release = matrices["release"]
    cases += expand_release_cases(
        seeds=seeds, workloads=release["workloads"],
        n_tile_values=release["n_tiles"], ratios=release["delta_over_job"],
        releases=release["release"])
    status = matrices["status"]
    cases += expand_status_cases(seeds=seeds, scenarios=status["scenarios"])
    late = matrices["late"]
    cases += expand_late_cases(
        seeds=seeds, workloads=late["workloads"], n_tiles=late["n_tiles"][0],
        ratios=late["ratios"], policies=late["policies"],
        window_modes=late["window_mode"])
    ledger = matrices["ledger"]
    cases += expand_ledger_cases(workloads=ledger["workloads"],
                                 n_tile_values=ledger["n_tiles"])
    return cases


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=str(DEFAULT_CONFIG))
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--out", default="")
    args = parser.parse_args()
    config = yaml.safe_load(Path(args.config).read_text())
    calibration = ROOT / config["calibration"]
    cases = build_cases(config)
    if args.limit:
        cases = cases[:args.limit]
    out = Path(args.out) if args.out else ROOT / config["outputs"]["cases"]
    print(f"L2_CASES={len(cases)}")
    payloads = [(case.__dict__, str(calibration)) for case in cases]
    rows: list[dict] = []
    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as executor:
        futures = [executor.submit(evaluate_one, payload) for payload in payloads]
        for index, future in enumerate(as_completed(futures), start=1):
            rows.append(future.result())
            if index % 5000 == 0 or index == len(futures):
                print(f"[{index}/{len(futures)}]")
    rows.sort(key=lambda row: (row["kind"], row["case_id"]))
    fieldnames = sorted({key for row in rows for key in row})
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    counts: dict[str, int] = {}
    for row in rows:
        counts[row["kind"]] = counts.get(row["kind"], 0) + 1
    summary = {
        "cases": len(rows),
        "by_kind": counts,
        "jobs": args.jobs,
        "correct": sum(1 for row in rows if row.get("correct") in (1, "1")),
        "incorrect": sum(1 for row in rows if row.get("correct") in (0, "0")),
        "status_counts": {},
    }
    for row in rows:
        if row.get("status"):
            summary["status_counts"][row["status"]] = \
                summary["status_counts"].get(row["status"], 0) + 1
    summary_path = ROOT / "results/phase2/v21/l2/matrix_summary.json"
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(f"L2_OUT={out.relative_to(ROOT)}")
    print(f"L2_CORRECT={summary['correct']} L2_INCORRECT={summary['incorrect']}")
    print(f"L2_STATUS={json.dumps(summary['status_counts'], sort_keys=True)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
