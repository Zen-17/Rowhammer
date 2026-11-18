#!/usr/bin/env python3
"""Aggregate G14 supplement runs into G15 figure-data CSVs and statistics."""
from __future__ import annotations

import csv
import json
import math
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "configs/generated/supplement-manifest.json"
RUN_ROOT = ROOT / "runs/supplement"
RESULTS = ROOT / "results/supplement"
FIG_DATA = RESULTS / "figure_data"
FIG3 = ROOT / "results/figure_data/fig3_control_window.csv"
STAT_SEED = 20261115

SCENARIOS = [
    "S1_SINGLE", "S2_DIFF_CW", "S3_DIFF_BURST_SAME_BANK",
    "S4_SAME_CW_TWO_BITS", "S5_DIFF_BANK", "S6_SAME_BIT_RECUR",
]
FAULT_ORDER = [
    ("F3M", "NONE"), ("F4S", "NONE"),
    ("F1W_F4S", "SAME_BANK"), ("F1W_F4S", "DIFF_BANK"),
]


def read_csv(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def write_csv(path: Path, rows: list[dict], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def bool_text(value) -> str:
    if value is None:
        return ""
    return "True" if bool(value) else "False"


def wilson(k: int, n: int) -> tuple[float | None, float | None]:
    if n == 0:
        return None, None
    z = 1.96
    p = k / n
    denom = 1.0 + z * z / n
    center = (p + z * z / (2 * n)) / denom
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom
    return max(0.0, center - half), min(1.0, center + half)


def signature_from_reports(rows: list[dict]) -> tuple[str, str]:
    first_cycle = int(rows[0]["report_cycle"]) if rows else 0
    signature = ";".join(
        f"{row['error_class']}:{row['region_id']}" for row in rows)
    timing = ";".join(
        f"{row['error_class']}:{row['region_id']}@{int(row['report_cycle']) - first_cycle}"
        for row in rows)
    return signature, timing


def p1g_rows(manifest: dict) -> list[dict]:
    rows = []
    for case in manifest["cases"]:
        if case["group"] != "P1-G Escape":
            continue
        run_dir = Path(case["case"]["out"])
        metrics_path = run_dir / "metrics.json"
        if not metrics_path.exists():
            continue
        metrics = json.loads(metrics_path.read_text())
        meta = case["case"]
        rows.append({
            "n_tiles": meta["n_tiles"],
            "consumer": meta["consumer"],
            "seed": meta["seed"],
            "fault_kind": meta["fault_kind"],
            "escape_placement": meta["escape_placement"],
            "delay_ratio": meta["delay_ratio"],
            "hold_ratio": meta["hold_ratio"],
            "transport_delay_cycles": metrics.get("transport_delay_cycles"),
            "hold_cycles": metrics.get("hold_cycles"),
            "outcome_class": metrics.get("outcome_class"),
            "escape_outcome": metrics.get("escape_outcome"),
            "corruption_propagates": metrics.get("corruption_propagates"),
            "held_output_corrupt": metrics.get("held_output_corrupt"),
            "published": metrics.get("published"),
            "consumed": metrics.get("consumed"),
            "output_changed": metrics.get("output_changed"),
            "report_covers_corrupt_bank": metrics.get("report_covers_corrupt_bank"),
        })
    return rows


def p1h_rows(manifest: dict) -> tuple[list[dict], dict]:
    rows = []
    by_key = {}
    for case in manifest["cases"]:
        if case["group"] != "P1-H Sequence":
            continue
        run_dir = Path(case["case"]["out"])
        metrics_path = run_dir / "metrics.json"
        if not metrics_path.exists():
            continue
        metrics = json.loads(metrics_path.read_text())
        reports = read_csv(run_dir / "reports.csv")
        signature, timing = signature_from_reports(reports)
        meta = case["case"]
        row = {
            "seed": meta["seed"],
            "scenario": meta["scenario"],
            "granularity": meta["granularity"],
            "scrub": meta["scrub"],
            "bank_p": meta["bank_p"],
            "bank_r": meta["bank_r"],
            "detected_events": metrics.get("detected_events"),
            "delivered_reports": metrics.get("delivered_reports"),
            "internal_ce": metrics.get("internal_ce"),
            "internal_ue": metrics.get("internal_ue"),
            "scrub_writes": metrics.get("scrub_writes"),
            "report_signature": signature,
            "report_signature_with_timing": timing,
            "contract_pass": metrics.get("contract_pass"),
            "timeout": metrics.get("timeout"),
            "reports": reports,
        }
        rows.append(row)
        by_key[(meta["granularity"], int(meta["scrub"]), meta["scenario"],
                meta["seed"])] = row
    return rows, by_key


def build_pair_rows(by_key: dict) -> tuple[list[dict], dict]:
    rows = []
    matrix = {}
    for granularity in ("bank", "page", "codeword"):
        for scrub in (0, 1):
            for i, first in enumerate(SCENARIOS):
                for second in SCENARIOS[i + 1:]:
                    seeds = []
                    equal = 0
                    timing_equal = 0
                    for seed in range(10):
                        a = by_key.get((granularity, scrub, first, seed))
                        b = by_key.get((granularity, scrub, second, seed))
                        if a is None or b is None:
                            continue
                        seeds.append(seed)
                        equal += int(a["report_signature"] == b["report_signature"])
                        timing_equal += int(
                            a["report_signature_with_timing"]
                            == b["report_signature_with_timing"])
                    note = ""
                    if not seeds:
                        note = "N/A: S6 is not run at scrub=0"
                    row = {
                        "granularity": granularity,
                        "scrub": scrub,
                        "scenario_i": first,
                        "scenario_j": second,
                        "indistinguishable_seeds": equal if seeds else "",
                        "n_seeds": len(seeds),
                        "identical_with_timing_seeds": timing_equal if seeds else "",
                        "note": note,
                    }
                    rows.append(row)
                    matrix[(granularity, scrub, first, second)] = (
                        equal / len(seeds) if seeds else None)
    return rows, matrix


def build_cof_rows(by_key: dict) -> list[dict]:
    rows = []
    for granularity in ("bank", "page", "codeword"):
        for scrub in (0, 1):
            for scenario in SCENARIOS:
                seeds = [seed for seed in range(10)
                         if (granularity, scrub, scenario, seed) in by_key]
                cof = 0
                ue = 0
                for seed in seeds:
                    reports = by_key[(granularity, scrub, scenario, seed)]["reports"]
                    ce_regions = Counter(row["region_id"] for row in reports
                                         if row["error_class"] == "CE")
                    cof += int(any(count >= 2 for count in ce_regions.values()))
                    ue += int(any(row["error_class"] == "UE" for row in reports))
                rows.append({
                    "granularity": granularity,
                    "scrub": scrub,
                    "scenario": scenario,
                    "cof_fire_seeds": cof if seeds else "",
                    "ue_seen_seeds": ue if seeds else "",
                    "n_seeds": len(seeds),
                    "note": "" if seeds else "N/A: S6 is not run at scrub=0",
                })
    return rows


def build_statistics(p1g: list[dict]) -> list[dict]:
    fig3_rows = [row for row in read_csv(FIG3)
                 if str(row.get("control", "")).lower() != "true"
                 and row.get("profile") == "LOCAL_VISIBLE"]
    fig3 = defaultdict(list)
    for row in fig3_rows:
        fig3[(int(row["n_tiles"]), row["consumer"], float(row["delay_ratio"]),
              float(row["hold_ratio"]))].append(row)
    groups = defaultdict(list)
    for row in p1g:
        key = (int(row["n_tiles"]), row["consumer"], row["fault_kind"],
               row["escape_placement"], float(row["delay_ratio"]),
               float(row["hold_ratio"]))
        groups[key].append(row)

    stats = []
    for key in sorted(groups, key=lambda k: (k[0], k[1] != "HOST", FAULT_ORDER.index(
            (k[2], k[3])), k[4], k[5])):
        n_tiles, consumer, fault, placement, delay_ratio, hold_ratio = key
        rows = groups[key]
        total = len(rows)
        masked = sum(1 for row in rows if row["escape_outcome"] == "MASKED")
        prevented = sum(1 for row in rows if row["escape_outcome"] == "PREVENTED")
        consumed = sum(1 for row in rows if row["escape_outcome"] == "CORRUPT_CONSUMED")
        n_prime = total - masked
        fig3_cell = fig3.get((n_tiles, consumer, delay_ratio, hold_ratio), [])
        timely = sum(1 for row in fig3_cell if row["outcome_class"] == "TIMELY_CONTROL")
        timely_n = len(fig3_cell)
        timely_low, timely_high = wilson(timely, timely_n)
        timely_frac = timely / timely_n if timely_n else None
        base = {
            "figure": "fig7_figS2",
            "group": "P1-G Escape",
            "config": f"{fault}/{placement}",
            "n_tiles": n_tiles,
            "consumer": consumer,
            "fault_kind": fault,
            "escape_placement": placement,
            "delay_ratio": delay_ratio,
            "hold_ratio": hold_ratio,
            "n_samples": total,
            "n_seeds": total,
            "masked": masked,
            "total": total,
            "median": "",
            "min": "",
            "max": "",
            "ci_method": "wilson_95",
            "fig3_timely_fraction": "",
            "fig3_timely_n": "",
            "fig3_timely_low": "",
            "fig3_timely_high": "",
            "prevented_minus_timely": "",
        }
        for metric, numerator, denominator in (
                ("PREVENTED_fraction", prevented, n_prime),
                ("CORRUPT_CONSUMED_fraction", consumed, n_prime),
                ("MASKED_fraction", masked, total)):
            low, high = wilson(numerator, denominator)
            row = dict(base)
            row.update({
                "stat_id": f"supp_{metric}_{n_tiles}_{consumer}_{fault}_"
                           f"{placement}_D{delay_ratio:g}_H{hold_ratio:g}",
                "metric": metric,
                "numerator": numerator,
                "denominator": denominator,
                "mean": (numerator / denominator) if denominator else "",
                "ci_low": low if low is not None else "",
                "ci_high": high if high is not None else "",
            })
            if metric == "PREVENTED_fraction" and fault == "F3M":
                row.update({
                    "fig3_timely_fraction": timely_frac if timely_frac is not None else "",
                    "fig3_timely_n": timely_n,
                    "fig3_timely_low": timely_low if timely_low is not None else "",
                    "fig3_timely_high": timely_high if timely_high is not None else "",
                    "prevented_minus_timely": (
                        (numerator / denominator - timely_frac)
                        if denominator and timely_frac is not None else ""),
                })
            stats.append(row)
    return stats


def main() -> int:
    manifest = json.loads(MANIFEST.read_text())
    p1g = p1g_rows(manifest)
    p1h, by_key = p1h_rows(manifest)
    if len(p1g) != 4320 or len(p1h) != 330:
        raise SystemExit(f"unexpected data rows: P1G={len(p1g)} P1H={len(p1h)}")

    fig7_fields = [
        "n_tiles", "consumer", "seed", "fault_kind", "escape_placement",
        "delay_ratio", "hold_ratio", "transport_delay_cycles", "hold_cycles",
        "outcome_class", "escape_outcome", "corruption_propagates",
        "held_output_corrupt", "published", "consumed", "output_changed",
        "report_covers_corrupt_bank",
    ]
    fig7_rows = []
    for row in p1g:
        fig7_rows.append({
            **{key: row[key] for key in fig7_fields},
            "corruption_propagates": bool_text(row["corruption_propagates"]),
            "held_output_corrupt": bool_text(row["held_output_corrupt"]),
            "published": bool_text(row["published"]),
            "consumed": bool_text(row["consumed"]),
            "output_changed": bool_text(row["output_changed"]),
            "report_covers_corrupt_bank": bool_text(row["report_covers_corrupt_bank"]),
        })
    write_csv(FIG_DATA / "fig7_escape.csv", fig7_rows, fig7_fields)

    fig8_signature_fields = [
        "seed", "scenario", "granularity", "scrub", "bank_p", "bank_r",
        "detected_events", "delivered_reports", "internal_ce", "internal_ue",
        "scrub_writes", "report_signature", "report_signature_with_timing",
        "contract_pass", "timeout",
    ]
    write_csv(FIG_DATA / "fig8_signatures.csv", [
        {key: row[key] for key in fig8_signature_fields} for row in p1h
    ], fig8_signature_fields)

    pair_rows, matrix = build_pair_rows(by_key)
    pair_fields = ["granularity", "scrub", "scenario_i", "scenario_j",
                   "indistinguishable_seeds", "n_seeds",
                   "identical_with_timing_seeds", "note"]
    write_csv(FIG_DATA / "fig8_pairs.csv", pair_rows, pair_fields)

    cof_rows = build_cof_rows(by_key)
    cof_fields = ["granularity", "scrub", "scenario", "cof_fire_seeds",
                  "ue_seen_seeds", "n_seeds", "note"]
    write_csv(FIG_DATA / "fig8_cof_rule.csv", cof_rows, cof_fields)

    stats = build_statistics(p1g)
    stat_fields = [
        "stat_id", "figure", "metric", "group", "config", "n_tiles", "consumer",
        "fault_kind", "escape_placement", "delay_ratio", "hold_ratio",
        "n_samples", "n_seeds", "numerator", "denominator", "mean", "median",
        "min", "max", "ci_low", "ci_high", "ci_method", "masked", "total",
        "fig3_timely_fraction", "fig3_timely_n", "fig3_timely_low",
        "fig3_timely_high", "prevented_minus_timely",
    ]
    write_csv(RESULTS / "statistics_supplement.csv", stats, stat_fields)

    validation_path = RESULTS / "validation.json"
    validation = json.loads(validation_path.read_text()) if validation_path.exists() else {}
    reconciliation = {
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "manifest_total": manifest["expected_total"],
        "manifest_by_group": manifest["expected_by_group"],
        "executed_p1g": len(p1g),
        "executed_p1h": len(p1h),
        "runner_counts": validation.get("counts"),
        "runner_pass": validation.get("pass"),
        "missing": manifest["expected_total"] - len(p1g) - len(p1h),
        "figure_data": {
            "fig7_escape": len(fig7_rows),
            "fig8_signatures": len(p1h),
            "fig8_pairs": len(pair_rows),
            "fig8_cof_rule": len(cof_rows),
            "statistics_supplement": len(stats),
        },
    }
    (RESULTS / "reconciliation_supplement.json").write_text(
        json.dumps(reconciliation, indent=2) + "\n")
    print(f"P1G rows={len(p1g)} P1H rows={len(p1h)} stats={len(stats)}")
    print(f"G15_AGGREGATE={'PASS' if reconciliation['missing'] == 0 and validation.get('pass') else 'FAIL'}")
    return 0 if reconciliation["missing"] == 0 and validation.get("pass") else 1


if __name__ == "__main__":
    raise SystemExit(main())
