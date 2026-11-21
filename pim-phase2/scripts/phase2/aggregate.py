#!/usr/bin/env python3
"""Aggregate L2 case rows into figure_data and statistics.csv.

Plotting never reruns the evaluator and only reads
``results/phase2/figure_data/*.csv``.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Iterable, List

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
P1_ROOT = ROOT.parent / "pim-phase1"
CASES = ROOT / "results/phase2/l2/cases.csv"
FIGDATA = ROOT / "results/phase2/figure_data"
STATS = ROOT / "results/phase2/statistics.csv"
FIG6 = P1_ROOT / "results/figure_data/fig6_cost.csv"
STAT_SEED = 20261115
BOOTSTRAP_N = 10000


def read_csv(path: Path) -> List[dict]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def write_csv(path: Path, rows: List[dict], fieldnames: List[str] | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = fieldnames or (list(rows[0].keys()) if rows else [])
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def as_float(value) -> float | None:
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def as_int(value) -> int | None:
    number = as_float(value)
    return None if number is None else int(number)


def bootstrap_ci(values: List[float]) -> tuple[float | None, float | None]:
    if len(values) < 2:
        return None, None
    rng = np.random.Generator(np.random.PCG64(STAT_SEED))
    data = np.asarray(values, dtype=float)
    means = np.empty(BOOTSTRAP_N)
    for index in range(BOOTSTRAP_N):
        means[index] = data[rng.integers(0, len(data), len(data))].mean()
    return float(np.quantile(means, 0.025)), float(np.quantile(means, 0.975))


def wilson(k: int, n: int) -> tuple[float | None, float | None, float | None]:
    if n == 0:
        return None, None, None
    z = 1.96
    p = k / n
    denom = 1.0 + z * z / n
    center = (p + z * z / (2 * n)) / denom
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom
    return p, max(0.0, center - half), min(1.0, center + half)


def add_continuous(rows: List[dict], figure: str, metric: str, group: str,
                   config: str, values: Iterable[float], source: str = "L2") -> None:
    clean = [v for v in values if v is not None]
    if not clean:
        return
    low, high = bootstrap_ci(clean)
    rows.append({
        "figure": figure, "metric": metric, "group": group, "config": config,
        "n": len(clean), "mean": statistics.mean(clean),
        "median": statistics.median(clean), "min": min(clean), "max": max(clean),
        "ci_low": "" if low is None else low,
        "ci_high": "" if high is None else high,
        "ci_method": "bootstrap_mean_95", "numerator": "", "denominator": "",
        "source": source,
    })


def add_proportion(rows: List[dict], figure: str, metric: str, group: str,
                   config: str, k: int, n: int, source: str = "L2") -> None:
    p, low, high = wilson(k, n)
    if p is None:
        return
    rows.append({
        "figure": figure, "metric": metric, "group": group, "config": config,
        "n": n, "mean": p, "median": p, "min": "", "max": "",
        "ci_low": low, "ci_high": high, "ci_method": "wilson_95",
        "numerator": k, "denominator": n, "source": source,
    })


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", default=str(CASES))
    parser.add_argument("--figure-data", default=str(FIGDATA))
    parser.add_argument("--statistics", default=str(STATS))
    args = parser.parse_args()
    cases_path = Path(args.cases)
    figdata = Path(args.figure_data)
    rows = read_csv(cases_path)
    by_kind = defaultdict(list)
    for row in rows:
        by_kind[row["kind"]].append(row)
    figdata.mkdir(parents=True, exist_ok=True)
    stats: List[dict] = []

    # --- P2-F1: selectivity across granularity and policy (UE, n_tiles=32) ---
    main = by_kind["main"]
    baseline = {}
    for row in main:
        if row["policy"] == "B0" and row["error_class"] == "UE":
            key = (row["workload"], row["n_tiles"], row["granularity"],
                   row["transport_delay"], row["fault_operand"], row["seed"])
            baseline[key] = as_float(row["recovery_cycles"])
    f1_rows = []
    for row in main:
        if row["error_class"] != "UE" or row["n_tiles"] != "32":
            continue
        if row["policy"] not in ("B0", "B1", "B2", "Oracle"):
            continue
        job_tiles = as_float(row["job_tiles"]) or 1.0
        replay_tiles = as_float(row["replay_tiles"]) or 0.0
        key = (row["workload"], row["n_tiles"], row["granularity"],
               row["transport_delay"], row["fault_operand"], row["seed"])
        base = baseline.get(key)
        recovery = as_float(row["recovery_cycles"])
        f1_rows.append({
            "figure": "P2-F1", "workload": row["workload"],
            "n_tiles": int(row["n_tiles"]), "granularity": row["granularity"],
            "error_class": row["error_class"], "fault_operand": row["fault_operand"],
            "policy": row["policy"], "transport_delay": int(row["transport_delay"]),
            "seed": int(row["seed"]),
            "replay_fraction": replay_tiles / job_tiles if job_tiles else 0.0,
            "recovery_cycles": recovery,
            "recovery_norm": (recovery / base) if base else "",
            "status": row["status"], "correct": int(row["correct"]),
            "affected_ops": int(row["affected_ops"]),
            "replay_tiles": int(replay_tiles), "job_tiles": int(job_tiles),
            "source": "L2",
        })
    write_csv(figdata / "p2f1_selectivity.csv", f1_rows)
    grouped = defaultdict(list)
    for row in f1_rows:
        grouped[(row["workload"], row["granularity"], row["policy"])].append(row)
    for (workload, granularity, policy), group in sorted(grouped.items()):
        add_continuous(stats, "P2-F1", "replayed_fraction",
                       f"{workload}/{granularity}", policy,
                       [row["replay_fraction"] for row in group])
        add_continuous(stats, "P2-F1", "recovery_norm",
                       f"{workload}/{granularity}", policy,
                       [row["recovery_norm"] for row in group
                        if row["recovery_norm"] != ""])

    # --- P2-F2: latency breakdown (W1, page granularity) ---
    f2_rows = []
    components = ("quiesce_cycles", "restore_cycles", "replay_cycles",
                  "resume_cycles", "publish_cycles")
    for row in main:
        if row["policy"] not in ("B0", "B1", "B2"):
            continue
        if row["workload"] != "W1" or row["granularity"] != "page":
            continue
        if row["n_tiles"] not in ("8", "32"):
            continue
        item = {
            "figure": "P2-F2", "workload": row["workload"],
            "n_tiles": int(row["n_tiles"]), "granularity": row["granularity"],
            "error_class": row["error_class"], "fault_operand": row["fault_operand"],
            "policy": row["policy"], "transport_delay": int(row["transport_delay"]),
            "seed": int(row["seed"]),
            "recovery_cycles": as_float(row["recovery_cycles"]),
            "replay_tiles": int(row["replay_tiles"]),
            "affected_ops": int(row["affected_ops"]),
            "status": row["status"], "correct": int(row["correct"]),
            "source": "L2",
        }
        for component in components:
            item[component] = as_float(row[component])
        f2_rows.append(item)
    write_csv(figdata / "p2f2_breakdown.csv", f2_rows)
    grouped = defaultdict(list)
    for row in f2_rows:
        grouped[(row["error_class"], row["n_tiles"], row["policy"])].append(row)
    for (error_class, n_tiles, policy), group in sorted(grouped.items()):
        for component in components + ("recovery_cycles",):
            add_continuous(stats, "P2-F2", component,
                           f"{error_class}/n{n_tiles}", policy,
                           [row[component] for row in group])

    # --- P2-F3: release overhead (L2) plus Phase 1 points ---
    f3_rows = []
    for row in by_kind["release"]:
        f3_rows.append({
            "figure": "P2-F3", "workload": row["workload"],
            "n_tiles": int(row["n_tiles"]), "release": row["release"],
            "delta_over_job": as_float(row["delta_over_job"]),
            "seed": int(row["seed"]),
            "overhead_fraction": as_float(row["overhead_fraction"]),
            "end_to_end_cycles": as_float(row["end_to_end_cycles"]),
            "baseline_end_to_end_cycles": as_float(row["baseline_end_to_end_cycles"]),
            "source": "L2",
        })
    if FIG6.exists():
        for row in read_csv(FIG6):
            if row["policy"] not in ("LOCAL_VISIBLE_H025T", "LOCAL_VISIBLE_H1T"):
                continue
            if row["consumer"] != "HOST":
                continue
            ratio = 0.25 if row["policy"].endswith("H025T") else 1.0
            f3_rows.append({
                "figure": "P2-F3", "workload": "W1",
                "n_tiles": int(row["n_tiles"]), "release": "GLOBAL_HOLD",
                "delta_over_job": ratio, "seed": int(row["seed"]),
                "overhead_fraction": as_float(row["overhead_fraction"]),
                "end_to_end_cycles": as_float(row["end_to_end_cycles"]),
                "baseline_end_to_end_cycles": as_float(row["paired_ext_off_end_to_end"]),
                "source": "P1-F",
            })
    write_csv(figdata / "p2f3_release.csv", f3_rows)
    grouped = defaultdict(list)
    for row in f3_rows:
        if row["source"] != "L2":
            continue
        key = (row["workload"], row["n_tiles"], row["release"], row["delta_over_job"])
        grouped[key].append(row["overhead_fraction"])
    for (workload, n_tiles, release, ratio), values in sorted(grouped.items()):
        add_continuous(stats, "P2-F3", "overhead_fraction",
                       f"{workload}/n{n_tiles}", f"{release}@{ratio}", values)

    # --- P2-F4: radius Pareto ---
    f4_rows = []
    for row in by_kind["radius"]:
        job_tiles = as_float(row["job_tiles"]) or 1.0
        f4_rows.append({
            "figure": "P2-F4", "distance_class": row["distance_class"],
            "policy": row["policy"], "seed": int(row["seed"]),
            "n_tiles": int(row["n_tiles"]),
            "silent_repaired": int(row["silent_repaired"]),
            "replay_tiles": int(row["replay_tiles"]),
            "replay_fraction": (as_float(row["replay_tiles"]) or 0.0) / job_tiles,
            "recovery_cycles": as_float(row["recovery_cycles"]),
            "correct": int(row["correct"]), "source": "L2",
        })
    write_csv(figdata / "p2f4_radius.csv", f4_rows)
    grouped = defaultdict(list)
    for row in f4_rows:
        grouped[(row["distance_class"], row["policy"])].append(row)
    for (distance, policy), group in sorted(grouped.items()):
        add_proportion(stats, "P2-F4", "silent_coverage", distance, policy,
                       sum(row["silent_repaired"] for row in group), len(group))
        add_continuous(stats, "P2-F4", "replay_fraction", distance, policy,
                       [row["replay_fraction"] for row in group])
        add_continuous(stats, "P2-F4", "recovery_cycles", distance, policy,
                       [row["recovery_cycles"] for row in group])

    # --- P2-F5: status distribution ---
    f5_rows = []
    for row in by_kind["status"]:
        f5_rows.append({
            "figure": "P2-F5", "scenario": row["status_scenario"],
            "seed": int(row["seed"]), "status": row["status"],
            "correct": int(row["correct"]), "escaped_bad": int(row["escaped_bad"]),
            "poisoned": int(row["poisoned"]), "source": "L2",
        })
    write_csv(figdata / "p2f5_status.csv", f5_rows)
    grouped = defaultdict(list)
    for row in f5_rows:
        grouped[(row["scenario"], row["status"])].append(row)
    scenario_totals = defaultdict(int)
    for row in f5_rows:
        scenario_totals[row["scenario"]] += 1
    for (scenario, status), group in sorted(grouped.items()):
        add_proportion(stats, "P2-F5", "status_share", scenario, status,
                       len(group), scenario_totals[scenario])

    # --- P2-F6: L2 prediction vs L3 measurement (filled by refit_l3.py) ---
    f6_path = figdata / "p2f6_validation.csv"
    if f6_path.exists():
        f6_rows = read_csv(f6_path)
        if f6_rows:
            grouped_f6 = defaultdict(list)
            for row in f6_rows:
                grouped_f6[(row["workload"], row["split"])].append(row)
            for (workload, split), group in sorted(grouped_f6.items()):
                add_continuous(stats, "P2-F6", "recovery_cycles", f"{workload}/{split}",
                               "L2_vs_L3", [as_float(row["l3_recovery_cycles"])
                                            for row in group], source="L2/L3")
                add_continuous(stats, "P2-F6", "absolute_relative_error",
                               f"{workload}/{split}", "L2_vs_L3",
                               [abs(as_float(row["relative_error"])) for row in group
                                if as_float(row["relative_error"]) is not None],
                               source="L2/L3")
    else:
        write_csv(f6_path, [], ["figure", "workload", "n_tiles", "granularity",
                                "error_class", "policy", "seed", "l2_recovery_cycles",
                                "l3_recovery_cycles", "relative_error", "split",
                                "source"])

    # --- P2-T1: ledger overhead ---
    t1_rows = []
    for row in by_kind["ledger"]:
        t1_rows.append({
            "figure": "P2-T1", "workload": row["workload"],
            "n_tiles": int(row["n_tiles"]), "job_tiles": int(row["job_tiles"]),
            "release": row["release"], "delta_over_job": as_float(row["delta_over_job"]),
            "seed": 0,
            "entries": int(row["ledger_entries"]),
            "bytes_est": int(row["ledger_bytes_est"]),
            "peak_held_items": int(row["peak_held_items"]), "source": "L2",
        })
    write_csv(figdata / "p2t1_ledger.csv", t1_rows)
    grouped = defaultdict(list)
    for row in t1_rows:
        grouped[(row["workload"], row["n_tiles"], row["release"],
                 row["delta_over_job"])].append(row)
    for (workload, n_tiles, release, ratio), group in sorted(grouped.items()):
        for metric in ("entries", "bytes_est", "peak_held_items"):
            add_continuous(stats, "P2-T1", metric, f"{workload}/n{n_tiles}",
                           f"{release}@{ratio}", [row[metric] for row in group])

    write_csv(Path(args.statistics), stats,
              ["figure", "metric", "group", "config", "n", "mean", "median",
               "min", "max", "ci_low", "ci_high", "ci_method", "numerator",
               "denominator", "source"])

    # --- validation of matrix completeness ---
    counts = {kind: len(rows) for kind, rows in sorted(by_kind.items())}
    checks = {
        "main_cases": counts.get("main", 0) == 45360,
        "radius_cases": counts.get("radius", 0) == 900,
        "release_cases": counts.get("release", 0) == 2160,
        "status_cases": counts.get("status", 0) == 210,
        "ledger_cases": counts.get("ledger", 0) == 36,
        "figure_data_rows": {
            "p2f1_selectivity.csv": len(f1_rows),
            "p2f2_breakdown.csv": len(f2_rows),
            "p2f3_release.csv": len(f3_rows),
            "p2f4_radius.csv": len(f4_rows),
            "p2f5_status.csv": len(f5_rows),
            "p2t1_ledger.csv": len(t1_rows),
        },
        "all_main_have_decisions": all(row.get("status") for row in main),
    }
    all_ok = (checks["main_cases"] and checks["radius_cases"]
              and checks["release_cases"] and checks["status_cases"]
              and checks["ledger_cases"] and checks["all_main_have_decisions"])
    payload = {
        "cases_file": str(cases_path.relative_to(ROOT)),
        "cases_sha256": hashlib.sha256(cases_path.read_bytes()).hexdigest(),
        "counts": counts,
        "checks": checks,
        "pass": all_ok,
    }
    (ROOT / "results/phase2/l2/aggregate_validation.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(f"P2F1={len(f1_rows)} P2F2={len(f2_rows)} P2F3={len(f3_rows)} "
          f"P2F4={len(f4_rows)} P2F5={len(f5_rows)} T1={len(t1_rows)}")
    print(f"COUNTS={json.dumps(counts, sort_keys=True)}")
    print(f"AGGREGATE={'PASS' if all_ok else 'FAIL'}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
