#!/usr/bin/env python3
"""Aggregate v2.1 L2/L3 results into figure data and statistics."""
from __future__ import annotations

import csv
import json
import math
import statistics
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CASES = ROOT / "results/phase2/v21/l2/cases.csv"
RUNS = ROOT / "results/phase2/v21/l3/run_metrics.csv"
FIGDATA = ROOT / "results/phase2/v21/figure_data"
STATS = ROOT / "results/phase2/v21/statistics.csv"
P1_FIG6 = ROOT.parent / "pim-phase1/results/figure_data/fig6_cost.csv"
MIX_A = {"same_row": 0.20, "plus1": 0.35, "minus1": 0.35,
         "plus2": 0.04, "minus2": 0.04, "far": 0.02}
MIX_B = {"same_row": 0.10, "plus1": 0.25, "minus1": 0.25,
         "plus2": 0.15, "minus2": 0.15, "far": 0.10}


def read_csv(path: Path) -> list[dict]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def write_csv(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()) if rows else [])
        writer.writeheader()
        writer.writerows(rows)


def num(value) -> float | None:
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def truthy(value) -> bool:
    return str(value).strip().lower() in ("1", "true", "yes")


def wilson(k: int, n: int) -> tuple[float, float, float]:
    if n == 0:
        return 0.0, 0.0, 0.0
    z = 1.96
    p = k / n
    denom = 1.0 + z * z / n
    center = (p + z * z / (2 * n)) / denom
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom
    return p, max(0.0, center - half), min(1.0, center + half)


def add_stat(rows: list[dict], figure: str, metric: str, group: str, config: str,
             values: list[float], source: str = "L2") -> None:
    clean = [v for v in values if v is not None]
    if not clean:
        return
    rows.append({
        "figure": figure, "metric": metric, "group": group, "config": config,
        "n": len(clean), "mean": statistics.mean(clean),
        "median": statistics.median(clean), "min": min(clean), "max": max(clean),
        "ci_low": "", "ci_high": "", "ci_method": "none",
        "numerator": "", "denominator": "", "source": source,
    })


def add_prop(rows: list[dict], figure: str, metric: str, group: str, config: str,
             k: int, n: int, source: str = "L2") -> None:
    p, low, high = wilson(k, n)
    rows.append({
        "figure": figure, "metric": metric, "group": group, "config": config,
        "n": n, "mean": p, "median": p, "min": "", "max": "",
        "ci_low": low, "ci_high": high, "ci_method": "wilson_95",
        "numerator": k, "denominator": n, "source": source,
    })


def main() -> int:
    cases = read_csv(CASES)
    runs = read_csv(RUNS) if RUNS.exists() else []
    by_kind = defaultdict(list)
    for row in cases:
        by_kind[row["kind"]].append(row)
    stats: list[dict] = []

    # P2-F1: affected/replayed tile fraction and fault overhead normalized to B0.
    main = by_kind["main"]
    b0_overhead = {}
    for row in main:
        if row["policy"] == "B0" and row["error_class"] == "UE":
            key = (row["workload"], row["n_tiles"], row["granularity"],
                   row["transport_delay"], row["fault_operand"], row["seed"])
            b0_overhead[key] = num(row["fault_overhead_cycles"])
    f1 = []
    for row in main:
        if row["error_class"] != "UE" or row["n_tiles"] != "32":
            continue
        if row["policy"] not in ("B0", "B1", "B2", "Oracle"):
            continue
        key = (row["workload"], row["n_tiles"], row["granularity"],
               row["transport_delay"], row["fault_operand"], row["seed"])
        base = b0_overhead.get(key)
        overhead = num(row["fault_overhead_cycles"])
        f1.append({
            "figure": "P2-F1", "workload": row["workload"],
            "n_tiles": int(row["n_tiles"]), "granularity": row["granularity"],
            "error_class": row["error_class"], "fault_operand": row["fault_operand"],
            "policy": row["policy"], "transport_delay": int(row["transport_delay"]),
            "seed": int(row["seed"]),
            "affected_fraction": (num(row["affected_ops"]) or 0.0)
            / (num(row["job_tiles"]) or 1.0),
            "replay_fraction": (num(row["replay_tiles"]) or 0.0)
            / (num(row["job_tiles"]) or 1.0),
            "fault_overhead_cycles": overhead,
            "fault_overhead_norm": (overhead / base) if base else "",
            "coalesced_extra_tiles": int(num(row["coalesced_extra_tiles"]) or 0),
            "status": row["status"], "correct": int(num(row["correct"]) or 0),
            "source": "L2",
        })
    write_csv(FIGDATA / "p2f1_selectivity.csv", f1)
    grouped = defaultdict(list)
    for row in f1:
        grouped[(row["workload"], row["granularity"], row["policy"],
                 row["transport_delay"])].append(row)
    for (workload, granularity, policy, delay), group in sorted(grouped.items()):
        add_stat(stats, "P2-F1", "affected_fraction",
                 f"{workload}/{granularity}/D{delay}", policy,
                 [row["affected_fraction"] for row in group])
        add_stat(stats, "P2-F1", "fault_overhead_norm",
                 f"{workload}/{granularity}/D{delay}", policy,
                 [row["fault_overhead_norm"] for row in group
                  if row["fault_overhead_norm"] != ""])

    # P2-F2: W1 page breakdown, total excludes resume.
    f2 = []
    for row in main:
        if row["workload"] != "W1" or row["granularity"] != "page":
            continue
        if row["policy"] not in ("B0", "B1", "B2"):
            continue
        if row["n_tiles"] not in ("8", "32"):
            continue
        item = {
            "figure": "P2-F2", "workload": "W1", "n_tiles": int(row["n_tiles"]),
            "granularity": "page", "error_class": row["error_class"],
            "fault_operand": row["fault_operand"], "policy": row["policy"],
            "transport_delay": int(row["transport_delay"]), "seed": int(row["seed"]),
            "quiesce_cycles": num(row["quiesce_cycles"]) or 0.0,
            "restore_cycles": num(row["restore_cycles"]) or 0.0,
            "replay_cycles": num(row["replay_cycles"]) or 0.0,
            "publish_cycles": num(row["publish_cycles"]) or 0.0,
            "resume_cycles": num(row["resume_cycles"]) or 0.0,
            "recovery_cycles": num(row["recovery_cycles"]) or 0.0,
            "recovery_cycles_excl_resume": num(row["recovery_cycles_excl_resume"]) or 0.0,
            "source": "L2",
        }
        f2.append(item)
    # L3 diamonds for the combinations that exist.
    for group_name, n_tiles, error_class, policy in (
            ("E2E-1", 8, "UE", "B1"), ("E2E-2", 8, "UE", "B0"),
            ("E2E-4", 8, "CE", "B2")):
        subset = [row for row in runs if row["group"] == group_name]
        if subset:
            f2.append({
                "figure": "P2-F2", "workload": "W1", "n_tiles": n_tiles,
                "granularity": "page", "error_class": error_class,
                "fault_operand": "mean", "policy": policy,
                "transport_delay": 1024, "seed": -1,
                "quiesce_cycles": 0.0, "restore_cycles": 0.0,
                "replay_cycles": 0.0, "publish_cycles": 0.0,
                "resume_cycles": 0.0, "recovery_cycles": 0.0,
                "recovery_cycles_excl_resume": statistics.mean(
                    float(row["recovery_cycles_excl_resume"]) for row in subset),
                "source": "L3",
            })
    for n_tiles in (8, 32):
        for error_class in ("CE", "UE"):
            for policy in ("B0", "B1", "B2"):
                subset = [row for row in runs if row["group"] == "SWEEP"
                          and row["workload"] == "W1"
                          and row["n_tiles"] == str(n_tiles)
                          and row["granularity"] == "page"
                          and row["fault_kind"]
                          == ("F1" if error_class == "CE" else "F2")
                          and row["policy"] == policy]
                if subset:
                    f2.append({
                        "figure": "P2-F2", "workload": "W1", "n_tiles": n_tiles,
                        "granularity": "page", "error_class": error_class,
                        "fault_operand": "mean", "policy": policy,
                        "transport_delay": 1024, "seed": -1,
                        "quiesce_cycles": 0.0, "restore_cycles": 0.0,
                        "replay_cycles": 0.0, "publish_cycles": 0.0,
                        "resume_cycles": 0.0, "recovery_cycles": 0.0,
                        "recovery_cycles_excl_resume": statistics.mean(
                            float(row["recovery_cycles_excl_resume"])
                            for row in subset),
                        "source": "L3",
                    })
    write_csv(FIGDATA / "p2f2_breakdown.csv", f2)
    grouped = defaultdict(list)
    for row in f2:
        if row["source"] == "L2":
            grouped[(row["error_class"], row["n_tiles"], row["policy"])].append(row)
    for (error_class, n_tiles, policy), group in sorted(grouped.items()):
        for component in ("quiesce_cycles", "restore_cycles", "replay_cycles",
                          "publish_cycles", "resume_cycles",
                          "recovery_cycles_excl_resume"):
            add_stat(stats, "P2-F2", component, f"{error_class}/n{n_tiles}",
                     policy, [row[component] for row in group])

    # P2-F3: release overhead plus P1-F and E2E-9.
    f3 = []
    for row in by_kind["release"]:
        f3.append({
            "figure": "P2-F3", "workload": row["workload"],
            "n_tiles": int(row["n_tiles"]), "release": row["release"],
            "delta_over_job": num(row["delta_over_job"]),
            "seed": int(row["seed"]),
            "overhead_fraction": num(row["overhead_fraction"]),
            "end_to_end_cycles": num(row["end_to_end_cycles"]),
            "baseline_end_to_end_cycles": num(row["baseline_end_to_end_cycles"]),
            "source": "L2",
        })
    if P1_FIG6.exists():
        for row in read_csv(P1_FIG6):
            if row["policy"] not in ("LOCAL_VISIBLE_H025T", "LOCAL_VISIBLE_H1T"):
                continue
            if row["consumer"] != "HOST":
                continue
            ratio = 0.25 if row["policy"].endswith("H025T") else 1.0
            f3.append({
                "figure": "P2-F3", "workload": "W1", "n_tiles": int(row["n_tiles"]),
                "release": "GLOBAL_HOLD", "delta_over_job": ratio,
                "seed": int(row["seed"]),
                "overhead_fraction": num(row["overhead_fraction"]),
                "end_to_end_cycles": num(row["end_to_end_cycles"]),
                "baseline_end_to_end_cycles": num(row["paired_ext_off_end_to_end"]),
                "source": "P1-F",
            })
    for row in runs:
        if row["group"] != "E2E-9":
            continue
        baseline = float(row["baseline_end_to_end_cycles"])
        end = float(row["end_to_end_cycles"])
        host_cost = 20.142857 + int(row["n_tiles"]) * 692.857143
        baseline += host_cost
        end += host_cost
        ratio = 1.0 if row["policy"] == "NONE" else ""
        release = "GLOBAL_HOLD" if "global" in row["run_id"] else "DEP_RELEASE"
        if "025" in row["run_id"]:
            ratio = 0.25
        elif "100" in row["run_id"]:
            ratio = 1.0
        elif "global" in row["run_id"]:
            ratio = 1.0
        f3.append({
            "figure": "P2-F3", "workload": "W1", "n_tiles": int(row["n_tiles"]),
            "release": release, "delta_over_job": ratio, "seed": -1,
            "overhead_fraction": (end - baseline) / baseline if baseline else "",
            "end_to_end_cycles": end, "baseline_end_to_end_cycles": baseline,
            "source": "L3",
        })
    write_csv(FIGDATA / "p2f3_release.csv", f3)
    grouped = defaultdict(list)
    for row in f3:
        if row["source"] == "L2":
            grouped[(row["workload"], row["n_tiles"], row["release"],
                     row["delta_over_job"])].append(row["overhead_fraction"])
    for (workload, n_tiles, release, ratio), values in sorted(grouped.items()):
        add_stat(stats, "P2-F3", "overhead_fraction", f"{workload}/n{n_tiles}",
                 f"{release}@{ratio}", values)

    # P2-F4: replay cost and expected coverage under MIX-A/MIX-B.
    radius = by_kind["radius"]
    policies = ["B2", "B2R-0", "B2R-1", "B2R-2", "B0"]
    distances = ["same_row", "plus1", "minus1", "plus2", "minus2", "far"]
    f4 = []
    for policy in policies:
        subset = [row for row in radius if row["policy"] == policy]
        replay = statistics.mean((num(row["replay_tiles"]) or 0.0)
                                 / (num(row["job_tiles"]) or 1.0) for row in subset)
        coverage = {}
        for distance in distances:
            group = [row for row in subset if row["distance_class"] == distance]
            coverage[distance] = (sum(truthy(row["silent_repaired"]) for row in group)
                                  / len(group)) if group else 0.0
        mix_a = (MIX_A["same_row"] * coverage["same_row"]
                 + MIX_A["plus1"] * coverage["plus1"]
                 + MIX_A["plus2"] * coverage["plus2"]
                 + MIX_A["far"] * coverage["far"])
        mix_b = (MIX_B["same_row"] * coverage["same_row"]
                 + MIX_B["plus1"] * coverage["plus1"]
                 + MIX_B["plus2"] * coverage["plus2"]
                 + MIX_B["far"] * coverage["far"])
        f4.append({"figure": "P2-F4", "policy": policy, "replay_fraction": replay,
                   "expected_coverage_mix_a": mix_a, "expected_coverage_mix_b": mix_b,
                   "same_row": coverage["same_row"], "plus1": coverage["plus1"],
                   "minus1": coverage["minus1"], "plus2": coverage["plus2"],
                   "minus2": coverage["minus2"], "far": coverage["far"],
                   "source": "L2"})
    write_csv(FIGDATA / "p2f4_radius.csv", f4)
    for row in f4:
        add_stat(stats, "P2-F4", "replay_fraction", row["policy"], "all",
                 [row["replay_fraction"]])
        add_stat(stats, "P2-F4", "expected_coverage_mix_a", row["policy"], "MIX-A",
                 [row["expected_coverage_mix_a"]])
        add_stat(stats, "P2-F4", "expected_coverage_mix_b", row["policy"], "MIX-B",
                 [row["expected_coverage_mix_b"]])

    # P2-F5: status distribution excluding B0.
    scenarios = ["NORMAL_UE", "CE", "DELTA_SHORT_PAGE_0.5",
                 "DELTA_SHORT_BANK_0.5", "DELTA_FULL_1.0",
                 "SAME_CYCLE_RELEASE", "CHECKPOINT_MISSING",
                 "LEDGER_TRUNCATED", "REPORT_OVERFLOW"]
    status_rows = [row for row in by_kind["status"] if row["policy"] != "B0"]
    f5 = []
    for scenario in scenarios:
        subset = [row for row in status_rows if row["status_scenario"] == scenario]
        total = len(subset)
        for status in sorted({row["status"] for row in subset}):
            count = sum(1 for row in subset if row["status"] == status)
            f5.append({"figure": "P2-F5", "scenario": scenario, "status": status,
                       "count": count, "total": total,
                       "correct": sum(1 for row in subset
                                      if truthy(row["correct"]) and row["status"] == status),
                       "source": "L2"})
            add_prop(stats, "P2-F5", "status_share", scenario, status, count, total)
    write_csv(FIGDATA / "p2f5_status.csv", f5)

    # P2-F7: late-report sweep.
    late = by_kind["late"]
    f7 = []
    for row in late:
        ratio = (num(row["transport_delay"]) or 0.0) / (885.0 + 32 * 408.0)
        f7.append({
            "figure": "P2-F7", "workload": row["workload"],
            "fan_in": int(num(row["fan_in"]) or 0),
            "fault_operand": row["fault_operand"], "policy": row["policy"],
            "window_mode": row["window_mode"], "seed": int(row["seed"]),
            "delta_over_tk1": ratio,
            "affected_fraction": (num(row["affected_ops"]) or 0.0)
            / (num(row["job_tiles"]) or 1.0),
            "fault_overhead_cycles": num(row["fault_overhead_cycles"]),
            "reader_partial_at_ack": int(num(row["reader_partial_at_ack"]) or 0),
            "source": "L2",
        })
    for row in runs:
        if row["group"] != "E2E-8":
            continue
        ratio = (6200.0 if "6200" in row["run_id"] else 8260.0) / (885.0 + 32 * 408.0)
        f7.append({
            "figure": "P2-F7", "workload": "W2", "fan_in": 0,
            "fault_operand": "A", "policy": "B1", "window_mode": "AUTO",
            "seed": 0, "delta_over_tk1": ratio,
            "affected_fraction": (num(row["affected_ops"]) or 0.0)
            / (3.0 * int(row["n_tiles"])),
            "fault_overhead_cycles": num(row["fault_overhead_cycles"]),
            "reader_partial_at_ack": int(num(row["reader_partial_at_ack"]) or 0),
            "source": "L3",
        })
    write_csv(FIGDATA / "p2f7_late.csv", f7)
    grouped = defaultdict(list)
    for row in f7:
        if row["source"] != "L2" or row["window_mode"] != "AUTO":
            continue
        grouped[(row["workload"], row["policy"], row["delta_over_tk1"])].append(row)
    for (workload, policy, ratio), group in sorted(grouped.items()):
        add_stat(stats, "P2-F7", "affected_fraction", f"{workload}/D{ratio:.3f}",
                 policy, [row["affected_fraction"] for row in group])
        add_stat(stats, "P2-F7", "fault_overhead_cycles",
                 f"{workload}/D{ratio:.3f}", policy,
                 [row["fault_overhead_cycles"] for row in group])

    # P2-T1: ledger.
    t1 = []
    for row in by_kind["ledger"]:
        t1.append({
            "figure": "P2-T1", "workload": row["workload"],
            "n_tiles": int(row["n_tiles"]), "job_tiles": int(row["job_tiles"]),
            "release": row["release"],
            "delta_over_job": num(row["delta_over_job"]), "seed": 0,
            "entries": int(row["ledger_entries"]),
            "bytes_est": int(row["ledger_bytes_est"]),
            "peak_held_items": int(row["peak_held_items"]), "source": "L2",
        })
    write_csv(FIGDATA / "p2t1_ledger.csv", t1)
    for row in t1:
        add_stat(stats, "P2-T1", "peak_held_items",
                 f"{row['workload']}/n{row['n_tiles']}",
                 f"{row['release']}@{row['delta_over_job']}",
                 [row["peak_held_items"]])

    write_csv(STATS, stats)
    counts = Counter(row["kind"] for row in cases)
    payload = {
        "cases_file": str(CASES.relative_to(ROOT)),
        "counts": dict(counts),
        "figure_rows": {path.name: len(read_csv(path))
                        for path in sorted(FIGDATA.glob("p2f*.csv"))},
        "pass": counts.get("main", 0) == 45360,
    }
    (ROOT / "results/phase2/v21/l2/aggregate_validation.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(f"COUNTS={json.dumps(dict(counts), sort_keys=True)}")
    print(f"FIG_ROWS={json.dumps(payload['figure_rows'], sort_keys=True)}")
    print(f"STATS={len(stats)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
