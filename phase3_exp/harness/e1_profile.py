"""Join observer output into the E1 tables.

Complete 1024-activation windows and partial tails are both stored.  The
partial flag is how later summaries keep tails out of the alarm rate.
"""
from __future__ import annotations

import csv
import json
from pathlib import Path

from .paths import RESULTS

COLUMNS = [
    "workload", "access_mode", "window_id", "bank_id", "partial",
    "max_row_act", "total_bank_act", "f_max", "f_p95", "f_p99",
    "mean_open_cycles", "p99_open_cycles", "exact_exceed", "mg_alarm",
    "mg_max_estimate",
]
SUMMARY_COLUMNS = [
    "workload", "access_mode", "status", "cycles", "host_transactions", "pim_items",
    "detection_windows", "min_complete_windows", "partial_windows",
    "detection_f_max", "detection_f_p99", "f_max_p99", "detection_exceed_frac", "mg_alarm_frac",
    "unique_rows_min", "unique_rows_max", "hottest_act_max",
    "partial_f_max", "partial_acts",
]
ORDER = ["W1", "W2", "W4", "GEMV", "DEC"]
MODES = ["streaming", "reuse"]


def _percentile(values: list, probability: float) -> float:
    ordered = sorted(float(value) for value in values)
    if not ordered:
        return 0.0
    index = min(len(ordered) - 1, max(0, int(round(probability * (len(ordered) - 1)))))
    return ordered[index]


def _one(obs_dir: Path, name: str, mode: str) -> tuple:
    meta_path = obs_dir / f"{name}.{mode}.meta.json"
    obs_path = obs_dir / f"{name}.{mode}.obs.json"
    profile = []
    summary = {
        "workload": name,
        "access_mode": mode,
        "status": "FAILED",
    }
    if not meta_path.exists() or not obs_path.exists():
        return profile, summary
    meta = json.loads(meta_path.read_text())
    obs = json.loads(obs_path.read_text())
    summary.update({
        "status": meta.get("status", "FAILED"),
        "cycles": meta.get("cycles", ""),
        "host_transactions": meta.get("host_transactions", ""),
        "pim_items": meta.get("pim_items", ""),
        "detection_windows": obs.get("detection_windows", 0),
        "min_complete_windows": obs.get("min_complete_windows", 0),
        "partial_windows": obs.get("partial_windows", 0),
        "detection_f_max": obs.get("detection_f_max", ""),
        "detection_f_p99": obs.get("detection_f_p99", ""),
        "detection_exceed_frac": obs.get("detection_exceed_frac", ""),
        "mg_alarm_frac": obs.get("mg_alarm_frac", ""),
        "partial_f_max": obs.get("partial_f_max", ""),
        "partial_acts": obs.get("partial_acts", ""),
    })
    banks = obs.get("banks") or []
    if banks:
        unique = [int(bank["unique_rows"]) for bank in banks]
        hottest = [int(bank["max_row_act"]) for bank in banks]
        summary["unique_rows_min"] = min(unique)
        summary["unique_rows_max"] = max(unique)
        summary["hottest_act_max"] = max(hottest)
    if summary["status"] != "SUCCESS":
        return profile, summary
    windows = obs.get("windows") or []
    complete = [window for window in windows if not window.get("partial")]
    if complete:
        hottest = [float(window["f_max"]) for window in complete]
        tails = [float(window["f_p99"]) for window in complete]
        summary["detection_f_max"] = max(hottest)
        summary["f_max_p99"] = _percentile(hottest, 0.99)
        summary["detection_f_p99"] = max(tails)
    for window in windows:
        profile.append({
            "workload": name,
            "access_mode": mode,
            "window_id": window["window_id"],
            "bank_id": window["bank_id"],
            "partial": window.get("partial", False),
            "max_row_act": window["max_row_act"],
            "total_bank_act": window["total_bank_act"],
            "f_max": window["f_max"],
            "f_p95": window["f_p95"],
            "f_p99": window["f_p99"],
            "mean_open_cycles": window["mean_open_cycles"],
            "p99_open_cycles": window["p99_open_cycles"],
            "exact_exceed": window.get("exact_exceed", False),
            "mg_alarm": window.get("mg_alarm", False),
            "mg_max_estimate": window.get("mg_max_estimate", 0),
        })
    return profile, summary


def assemble(obs_dir: Path) -> tuple:
    profile = []
    summary = []
    for name in ORDER:
        for mode in MODES:
            rows, row = _one(obs_dir, name, mode)
            profile.extend(rows)
            summary.append(row)
    return profile, summary


def write_tables(obs_dir: Path) -> tuple:
    profile, summary = assemble(obs_dir)
    RESULTS.mkdir(parents=True, exist_ok=True)
    profile_path = RESULTS / "E1_benign_profile.csv"
    summary_path = RESULTS / "E1_detection_summary.csv"
    with profile_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS)
        writer.writeheader()
        writer.writerows(profile)
    with summary_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=SUMMARY_COLUMNS, extrasaction="ignore", restval="")
        writer.writeheader()
        writer.writerows(summary)
    return profile_path, summary_path
