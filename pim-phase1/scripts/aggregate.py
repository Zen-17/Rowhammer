#!/usr/bin/env python3
"""Aggregate the executed paper manifest into reconciled tables and figure data."""
from __future__ import annotations

import argparse
import csv
import json
import math
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read_json(path: Path) -> dict:
    return json.loads(path.read_text())


def write_csv(path: Path, fieldnames: list[str], rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", default=str(ROOT / "runs/paper"))
    parser.add_argument("--manifest", default=str(ROOT / "configs/generated/paper-manifest.json"))
    parser.add_argument("--out", default=str(ROOT / "results"))
    args = parser.parse_args()
    runs = Path(args.runs).resolve()
    manifest = read_json(Path(args.manifest).resolve())
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    figure_data = out / "figure_data"
    figure_data.mkdir(parents=True, exist_ok=True)

    records = []
    missing = []
    statuses = Counter()
    for case in manifest["cases"]:
        run_dir = Path(case["case"]["out"])
        metadata_path = run_dir / "metadata.json"
        validation_path = run_dir / "validation.json"
        metrics_path = run_dir / "metrics.json"
        if not metadata_path.exists() or not validation_path.exists():
            missing.append(case["run_id"])
            statuses["MISSING"] += 1
            continue
        metadata = read_json(metadata_path)
        validation = read_json(validation_path)
        metrics = read_json(metrics_path) if metrics_path.exists() else {}
        status = metadata.get("run_status", "UNKNOWN")
        statuses[status] += 1
        records.append({
            "case": case,
            "metadata": metadata,
            "validation": validation,
            "metrics": metrics,
        })

    rows = []
    for record in records:
        case = record["case"]
        meta = record["case"]["case"]
        metrics = record["metrics"]
        rows.append({
            "run_id": case["run_id"],
            "group": case["group"],
            "case_id": meta.get("case_id", ""),
            "status": record["metadata"].get("run_status", ""),
            "pass": record["validation"].get("pass"),
            "expected_negative": case["expected_negative"],
            "n_tiles": meta.get("n_tiles", ""),
            "seed": meta.get("seed", ""),
            "consumer": meta.get("consumer", ""),
            "profile": meta.get("profile", case["params"].get("profile", "")),
            "path": meta.get("path", case["params"].get("path", "")),
            "policy": meta.get("policy", ""),
            "scenario": meta.get("scenario", ""),
            "delay_ratio": meta.get("delay_ratio", ""),
            "hold_ratio": meta.get("hold_ratio", ""),
            "transport_delay_cycles": metrics.get("transport_delay_cycles", ""),
            "hold_cycles": metrics.get("hold_cycles", ""),
            "outcome_class": metrics.get("outcome_class", ""),
            "contract_pass": metrics.get("contract_pass", record["validation"].get("pass")),
            "producer_cycles": metrics.get("producer_cycles", ""),
            "end_to_end_cycles": metrics.get("end_to_end_cycles", ""),
            "quiesce_cycles": metrics.get("quiesce_cycles", ""),
            "restore_cycles": metrics.get("restore_cycles", ""),
            "reexec_cycles": metrics.get("reexec_cycles", ""),
            "recovery_cycles": metrics.get("recovery_cycles", ""),
            "detected_events": metrics.get("detected_events", ""),
            "delivered_reports": metrics.get("delivered_reports", ""),
            "queue_drops": metrics.get("queue_drops", metrics.get("dropped_reports", "")),
            "injected_drops": metrics.get("injected_drops", ""),
            "output_bit_exact": metrics.get("output_bit_exact", ""),
            "published": metrics.get("published", ""),
            "consumed": metrics.get("consumed", ""),
            "error": record["validation"].get("error", ""),
        })
    fieldnames = list(rows[0].keys()) if rows else []
    write_csv(out / "run_metrics.csv", fieldnames, rows)
    write_csv(out / "failure_cases.csv", fieldnames,
              [row for row in rows if row["status"] in ("FAIL", "TIMEOUT")])

    summary = []
    for group in sorted({row["group"] for row in rows}):
        group_rows = [row for row in rows if row["group"] == group]
        summary.append({
            "group": group,
            "cases": len(group_rows),
            "success": sum(1 for row in group_rows if row["status"] == "SUCCESS"),
            "expected_negative": sum(
                1 for row in group_rows if row["status"] == "EXPECTED_NEGATIVE"),
            "fail": sum(1 for row in group_rows if row["status"] == "FAIL"),
            "timeout": sum(1 for row in group_rows if row["status"] == "TIMEOUT"),
            "contract_pass": sum(1 for row in group_rows if row["contract_pass"] is True),
        })
    write_csv(out / "summary.csv", list(summary[0].keys()) if summary else [], summary)

    fig1 = []
    for record in records:
        if record["case"]["group"] not in ("P1-A",):
            continue
        meta = record["case"]["case"]
        metrics = record["metrics"]
        fig1.append({
            "profile": meta["profile"],
            "path": meta["path"],
            "fault": meta["fault"],
            "bank": meta["bank"],
            "output_correct": metrics.get("output_equals_original"),
            "internal_ce": metrics.get("internal_ce"),
            "internal_ue": metrics.get("internal_ue"),
            "format_invalid": metrics.get("format_invalid"),
            "delivered_reports": metrics.get("delivered_reports"),
            "dropped_reports": metrics.get("dropped_reports"),
        })
    write_csv(figure_data / "fig1_path_coverage.csv",
              ["profile", "path", "fault", "bank", "output_correct", "internal_ce",
               "internal_ue", "format_invalid", "delivered_reports", "dropped_reports"], fig1)

    fig2 = []
    for record in records:
        if record["case"]["group"] != "P1-B":
            continue
        meta = record["case"]["case"]
        metrics = record["metrics"]
        latencies = metrics.get("report_latencies", [])
        fig2.append({
            "granularity": meta["granularity"],
            "bank": meta["bank"],
            "transport_delay_cycles": meta["delay"],
            "service_interval_cycles": meta["interval"],
            "path": meta["path"],
            "delivered_reports": metrics.get("delivered_reports"),
            "dropped_reports": metrics.get("dropped_reports"),
            "max_queue_depth": metrics.get("max_queue_depth"),
            "report_latency_min": min(latencies) if latencies else "",
            "report_latency_max": max(latencies) if latencies else "",
            "report_latency_mean": (sum(latencies) / len(latencies)) if latencies else "",
        })
    write_csv(figure_data / "fig2_report_latency.csv",
              ["granularity", "bank", "transport_delay_cycles", "service_interval_cycles",
               "path", "delivered_reports", "dropped_reports", "max_queue_depth",
               "report_latency_min", "report_latency_max", "report_latency_mean"], fig2)

    fig2_events = []
    for record in records:
        if record["case"]["group"] != "P1-B":
            continue
        meta = record["case"]["case"]
        if meta.get("path") != "HREAD_BURST":
            continue
        metrics = record["metrics"]
        run_dir = Path(record["case"]["case"]["out"])
        reports_path = run_dir / "reports.csv"
        if not reports_path.exists():
            continue
        with reports_path.open(newline="") as handle:
            reports = list(csv.DictReader(handle))
        latencies = metrics.get("report_latencies", [])
        for index, report in enumerate(reports):
            fig2_events.append({
                "granularity": meta["granularity"],
                "bank": meta["bank"],
                "transport_delay_cycles": meta["delay"],
                "service_interval_cycles": meta["interval"],
                "report_index": index + 1,
                "report_cycle": int(report["report_cycle"]),
                "latency_cycles": latencies[index] if index < len(latencies) else "",
                "cumulative_reports": index + 1,
                "region_type": report["region_type"],
                "region_id": report["region_id"],
                "error_class": report["error_class"],
            })
    write_csv(figure_data / "fig2_report_events.csv",
              ["granularity", "bank", "transport_delay_cycles", "service_interval_cycles",
               "report_index", "report_cycle", "latency_cycles", "cumulative_reports",
               "region_type", "region_id", "error_class"], fig2_events)

    fig3 = []
    for record in records:
        if record["case"]["group"] != "P1-C":
            continue
        meta = record["case"]["case"]
        metrics = record["metrics"]
        fig3.append({
            "n_tiles": meta["n_tiles"],
            "consumer": meta["consumer"],
            "seed": meta["seed"],
            "profile": meta.get("profile", "LOCAL_VISIBLE"),
            "control": meta.get("control", False),
            "delay_ratio": meta.get("delay_ratio", 0),
            "hold_ratio": meta.get("hold_ratio", 0),
            "transport_delay_cycles": meta.get("transport_delay_cycles", 0),
            "hold_cycles": meta.get("hold_cycles", 0),
            "outcome_class": metrics.get("outcome_class"),
            "output_changed": metrics.get("output_changed"),
            "published": metrics.get("published"),
            "consumed": metrics.get("consumed"),
            "quiesce_cycles": metrics.get("quiesce_cycles"),
        })
    write_csv(figure_data / "fig3_control_window.csv",
              ["n_tiles", "consumer", "seed", "profile", "control", "delay_ratio",
               "hold_ratio", "transport_delay_cycles", "hold_cycles", "outcome_class",
               "output_changed", "published", "consumed", "quiesce_cycles"], fig3)

    fig4 = []
    for record in records:
        if record["case"]["group"] != "P1-D":
            continue
        meta = record["case"]["case"]
        metrics = record["metrics"]
        fig4.append({
            "scenario": meta["scenario"],
            "channels": meta["channels"],
            "feeder_limit": meta["feeder_limit"],
            "handler_delay": meta["handler_delay"],
            "quiesce_cycles": metrics.get("quiesce_cycles"),
            "data_delta_after_ack": metrics.get("data_delta_after_ack"),
            "external_pending_frozen": metrics.get("external_pending_frozen"),
            "refresh_before": metrics.get("refresh_before"),
            "refresh_after": metrics.get("refresh_after"),
            "clock_delta": metrics.get("clock_delta"),
            "other_channel_data_delta": metrics.get("other_channel_data_delta"),
            "report_count": metrics.get("report_count"),
        })
    write_csv(figure_data / "fig4_quiescence.csv",
              ["scenario", "channels", "feeder_limit", "handler_delay", "quiesce_cycles",
               "data_delta_after_ack", "external_pending_frozen", "refresh_before",
               "refresh_after", "clock_delta", "other_channel_data_delta", "report_count"],
              fig4)

    fig5 = []
    for record in records:
        if record["case"]["group"] != "P1-E":
            continue
        case = record["case"]["case"]
        if case.get("kind") != "positive":
            continue
        metrics = record["metrics"]
        fig5.append({
            "n_tiles": case["n_tiles"],
            "seed": case["seed"],
            "checkpoint_bytes": metrics.get("checkpoint_bytes"),
            "restore_writes": metrics.get("restore_writes"),
            "restore_bytes": metrics.get("restore_bytes"),
            "restore_cycles": metrics.get("restore_cycles"),
            "reexec_cycles": metrics.get("reexec_cycles"),
            "recovery_cycles": metrics.get("recovery_cycles"),
            "quiesce_cycles": metrics.get("old_quiesce_cycles"),
            "output_bit_exact": metrics.get("output_bit_exact"),
            "new_state": metrics.get("new_state"),
        })
    write_csv(figure_data / "fig5_restore.csv",
              ["n_tiles", "seed", "checkpoint_bytes", "restore_writes", "restore_bytes",
               "restore_cycles", "reexec_cycles", "recovery_cycles", "quiesce_cycles",
               "output_bit_exact", "new_state"], fig5)

    cost = []
    baseline = {}
    wall_baseline = {}
    for record in records:
        if record["case"]["group"] != "P1-F Cost":
            continue
        meta = record["case"]["case"]
        metrics = record["metrics"]
        key = (meta["n_tiles"], meta["consumer"], meta["seed"])
        if meta["policy"] == "EXT_OFF":
            baseline[key] = metrics.get("end_to_end_cycles")
            wall_baseline[key] = record["metadata"].get("wall_seconds")
        cost.append((record, key))
    fig6 = []
    for record, key in cost:
        meta = record["case"]["case"]
        metrics = record["metrics"]
        base = baseline.get(key)
        end = metrics.get("end_to_end_cycles")
        overhead = (end / base - 1.0) if base and end else ""
        wall = record["metadata"].get("wall_seconds")
        wall_base = wall_baseline.get(key)
        wall_overhead = (wall / wall_base - 1.0) if wall and wall_base else ""
        fig6.append({
            "policy": meta["policy"],
            "n_tiles": meta["n_tiles"],
            "consumer": meta["consumer"],
            "seed": meta["seed"],
            "hold_cycles": metrics.get("hold_cycles"),
            "producer_cycles": metrics.get("producer_cycles"),
            "end_to_end_cycles": end,
            "paired_ext_off_end_to_end": base if base else "",
            "overhead_fraction": overhead,
            "checkpoint_bytes": metrics.get("checkpoint_bytes"),
            "peak_held_output_bytes": metrics.get("peak_held_output_bytes"),
            "parity_bytes": metrics.get("parity_bytes"),
            "runtime_metadata_bytes": metrics.get("runtime_metadata_bytes"),
            "output_bit_exact": metrics.get("output_bit_exact"),
            "wall_seconds": wall,
            "paired_ext_off_wall_seconds": wall_base if wall_base else "",
            "wall_overhead_fraction": wall_overhead,
        })
    write_csv(figure_data / "fig6_cost.csv",
              ["policy", "n_tiles", "consumer", "seed", "hold_cycles", "producer_cycles",
               "end_to_end_cycles", "paired_ext_off_end_to_end", "overhead_fraction",
               "checkpoint_bytes", "peak_held_output_bytes", "parity_bytes",
               "runtime_metadata_bytes", "output_bit_exact", "wall_seconds",
               "paired_ext_off_wall_seconds", "wall_overhead_fraction"], fig6)

    robust = []
    for record in records:
        if record["case"]["group"] != "P1-F Robustness":
            continue
        metrics = record["metrics"]
        robust.append({
            "queue_capacity": metrics.get("queue_capacity"),
            "service_interval_cycles": metrics.get("service_interval_cycles"),
            "drop_rate": metrics.get("drop_rate"),
            "total_detections": metrics.get("total_detections"),
            "forwarded_detections": metrics.get("forwarded_detections"),
            "delivered_reports": metrics.get("delivered_reports"),
            "queue_drops": metrics.get("queue_drops"),
            "injected_drops": metrics.get("injected_drops"),
            "overflow": metrics.get("overflow"),
            "max_queue_depth": metrics.get("max_queue_depth"),
        })
    write_csv(figure_data / "fig6_robustness.csv",
              ["queue_capacity", "service_interval_cycles", "drop_rate", "total_detections",
               "forwarded_detections", "delivered_reports", "queue_drops", "injected_drops",
               "overflow", "max_queue_depth"], robust)

    validation_summary = {
        "gate": "G8",
        "manifest_total": manifest["expected_total"],
        "executed_total": len(records),
        "missing": len(missing),
        "missing_run_ids": missing,
        "status_counts": dict(statuses),
        "expected_negative_declared": manifest["expected_negative"],
        "expected_negative_observed": statuses.get("EXPECTED_NEGATIVE", 0),
        "reconciliation": {
            "manifest_cases": manifest["expected_total"],
            "success": statuses.get("SUCCESS", 0),
            "expected_negative": statuses.get("EXPECTED_NEGATIVE", 0),
            "failure": statuses.get("FAIL", 0),
            "timeout": statuses.get("TIMEOUT", 0),
            "missing": statuses.get("MISSING", 0),
        },
        "groups": summary,
        "figure_data": sorted(path.name for path in figure_data.glob("*.csv")),
    }
    reconciled = (
        statuses.get("SUCCESS", 0) + statuses.get("EXPECTED_NEGATIVE", 0)
        + statuses.get("FAIL", 0) + statuses.get("TIMEOUT", 0)
        + statuses.get("MISSING", 0) == manifest["expected_total"]
    )
    validation_summary["pass"] = (
        reconciled and statuses.get("FAIL", 0) == 0 and statuses.get("TIMEOUT", 0) == 0
        and statuses.get("MISSING", 0) == 0
        and statuses.get("EXPECTED_NEGATIVE", 0) == manifest["expected_negative"])
    (out / "validation_summary.json").write_text(
        json.dumps(validation_summary, indent=2) + "\n")
    print(json.dumps(validation_summary, indent=2))
    print("G8_AGGREGATE=" + ("PASS" if validation_summary["pass"] else "FAIL"))
    return 0 if validation_summary["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
