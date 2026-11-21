#!/usr/bin/env python3
"""Run G4 P1-A/P1-B smoke cases through the all-path ECC/report harness."""
from __future__ import annotations

import csv
import hashlib
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIM_DIR = ROOT / "simulator"
SIM = SIM_DIR / "sim"
RUNS = ROOT / "runs/g4"
RESULTS = ROOT / "results/g4"
DEVICE_INI = SIM_DIR / "ini/HBM2_samsung_2M_16B_x64.ini"
SYSTEM_INI = ROOT / "configs/phase1_system_1ch.ini"
REPORT_HEADER = ["report_seq", "report_cycle", "error_class", "region_type", "region_id"]
TRUTH_HEADER = ["record_type", "detection_id", "fault_id", "fault_cycle", "storage_key",
                "codeword_index", "bit_positions", "raw_before", "raw_after",
                "target_access_id", "detect_cycle", "decoder_status", "eligible_report_id", "dropped"]
PROFILES = ["NATIVE", "LOCAL_VISIBLE", "LOCAL_HIDDEN", "HOST_ONLY"]
PATHS = ["HREAD", "PREAD", "PWRITE_HREAD", "PWRITE_PREAD"]
BANKS = [0, 3, 15]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def encode_address(channels: int, bank: int, row: int, column: int) -> int:
    # The 1-channel G4 fixture uses row=0, burst_column=0; keep the general
    # Scheme8 inverse here for page-region checks.
    rank_bits = 0
    row_bits = 14
    col_bits = 5
    bg_bits = 2
    bank_low_bits = 2
    chan_bits = {1: 0, 2: 1}[channels]
    offset_bits = 5
    bank_low = bank & ((1 << bank_low_bits) - 1)
    bg = bank >> bank_low_bits
    a = 0
    a = (a << row_bits) | row
    a = (a << col_bits) | column
    a = (a << bg_bits) | bg
    a = (a << bank_low_bits) | bank_low
    a = (a << chan_bits) | 0
    a = (a << offset_bits)
    return a


def case_params(case: dict) -> str:
    return f"""experiment=G4
profile={case['profile']}
path={case['path']}
fault_kind={case['fault']}
target_bank={case['bank']}
target_lane=0
data_bit_in_lane=15
fault_bit2=1
granularity={case['granularity']}
transport_delay_cycles={case['delay']}
service_interval_cycles={case['interval']}
report_queue_capacity=64
repeat_reads={case.get('repeat_reads', 32)}
watchdog_cycles=100000
device_ini={DEVICE_INI}
system_ini={SYSTEM_INI}
output_dir={case['out']}
"""


def build_cases() -> list[dict]:
    cases = []
    for profile in PROFILES:
        for path in PATHS:
            for fault in ("F0", "F1", "F2"):
                for bank in BANKS:
                    out = RUNS / "p1a" / f"{profile}_{path}_{fault}_bank{bank}"
                    cases.append({
                        "case_id": f"P1A_{profile}_{path}_{fault}_bank{bank}",
                        "profile": profile, "path": path, "fault": fault, "bank": bank,
                        "granularity": "bank", "delay": 0, "interval": 1, "out": out,
                    })
    for bank in BANKS:
        for granularity in ("bank", "page"):
            for delay in (0, 1, 16, 256, 4096):
                for interval in (1, 8):
                    out = RUNS / "p1b" / f"LOCAL_VISIBLE_HREAD_F1_bank{bank}_{granularity}_d{delay}_i{interval}"
                    cases.append({
                        "case_id": f"P1B_bank{bank}_{granularity}_d{delay}_i{interval}",
                        "profile": "LOCAL_VISIBLE", "path": "HREAD", "fault": "F1",
                        "bank": bank, "granularity": granularity, "delay": delay,
                        "interval": interval, "out": out,
                    })
    for bank in BANKS:
        out = RUNS / "p1b" / f"LOCAL_VISIBLE_HREAD_BURST_F1_bank{bank}_bank_d0_i8"
        cases.append({
            "case_id": f"P1B_BURST_bank{bank}_bank_d0_i8",
            "profile": "LOCAL_VISIBLE", "path": "HREAD_BURST", "fault": "F1",
            "bank": bank, "granularity": "bank", "delay": 0, "interval": 8,
            "repeat_reads": 32, "out": out,
        })
    return cases


def read_reports(path: Path) -> list[dict]:
    with path.open(newline="") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames != REPORT_HEADER:
            raise ValueError(f"reports header mismatch: {reader.fieldnames}")
        return list(reader)


def validate_case(case: dict) -> tuple[bool, dict, dict]:
    run = case["out"]
    metrics = json.loads((run / "metrics.json").read_text())
    reports = read_reports(run / "reports.csv")
    with (run / "truth.csv").open(newline="") as fh:
        truth_reader = csv.DictReader(fh)
        if truth_reader.fieldnames != TRUTH_HEADER:
            raise ValueError(f"truth header mismatch: {truth_reader.fieldnames}")
        truth = list(truth_reader)
    checks = {}
    if case["fault"] == "F0":
        checks["truth_empty"] = len(truth) == 0
    else:
        checks["truth_one_injection"] = len(truth) == 1 and truth[0]["record_type"] == "INJECTION"
        if truth:
            checks["truth_fault"] = truth[0]["fault_id"] == case["fault"]
            checks["truth_raw_changed"] = truth[0]["raw_before"] != truth[0]["raw_after"]
    checks["reports_from_detection"] = metrics["delivered_reports"] <= metrics["internal_ce"] + metrics["internal_ue"]
    checks["no_timeout"] = metrics["timeout"] is False
    checks["report_seq"] = [int(r["report_seq"]) for r in reports] == list(
        range(1, len(reports) + 1))
    if case["path"] == "HREAD_BURST":
        checks["internal_ce"] = metrics["internal_ce"] == 32
        checks["delivered"] = metrics["delivered_reports"] == 32
        checks["no_drops"] = metrics["dropped_reports"] == 0
        checks["report_count"] = len(reports) == 32
        checks["first_latency"] = metrics["report_latencies"][0] == max(1, case["delay"])
        if len(reports) > 1:
            gaps = [int(reports[i]["report_cycle"]) - int(reports[i - 1]["report_cycle"])
                    for i in range(1, len(reports))]
            checks["service_interval"] = all(g >= case["interval"] for g in gaps)
        checks["region"] = all(r["region_type"] == case["granularity"] for r in reports)
        checks["class"] = all(r["error_class"] == "CE" for r in reports)
        checks["output"] = metrics["output_equals_original"] is True
        return all(checks.values()), checks, metrics

    path = case["path"]
    profile = case["profile"]
    fault = case["fault"]
    expected_correct = True
    expected_ce = 0
    expected_ue = 0
    expected_delivered = 0
    expected_format = False
    expected_class = None
    if fault == "F0":
        if profile == "HOST_ONLY" and path == "PWRITE_HREAD":
            expected_correct = False
            expected_format = True
    elif fault == "F1":
        if profile in ("LOCAL_VISIBLE", "LOCAL_HIDDEN"):
            expected_ce = 1
            expected_delivered = 1 if profile == "LOCAL_VISIBLE" else 0
            expected_class = "CE"
        elif profile == "HOST_ONLY":
            if path == "HREAD":
                expected_ce = 1
                expected_delivered = 1
                expected_class = "CE"
            elif path == "PWRITE_HREAD":
                expected_correct = False
                expected_format = True
            else:
                expected_correct = False
        else:
            expected_correct = False
    elif fault == "F2":
        if profile in ("LOCAL_VISIBLE", "LOCAL_HIDDEN"):
            expected_correct = False
            expected_ue = 1
            expected_delivered = 1
            expected_class = "UE"
        elif profile == "HOST_ONLY":
            if path == "HREAD":
                expected_correct = False
                expected_ue = 1
                expected_delivered = 1
                expected_class = "UE"
            elif path == "PWRITE_HREAD":
                expected_correct = False
                expected_format = True
            else:
                expected_correct = False
        else:
            expected_correct = False

    checks["profile_path_fault_output"] = metrics["output_equals_original"] is expected_correct
    checks["internal_ce"] = metrics["internal_ce"] == expected_ce
    checks["internal_ue"] = metrics["internal_ue"] == expected_ue
    checks["delivered"] = metrics["delivered_reports"] == expected_delivered
    checks["format"] = bool(metrics["format_invalid"]) == expected_format
    checks["report_count"] = len(reports) == expected_delivered
    if expected_delivered:
        checks["class"] = all(r["error_class"] == expected_class for r in reports)
        expected_page = encode_address(1, case["bank"], 0, 0) // 4096
        expected_region_type = case["granularity"]
        expected_region_id = case["bank"] if expected_region_type == "bank" else expected_page
        checks["region_type"] = all(r["region_type"] == expected_region_type for r in reports)
        checks["region_id"] = all(int(r["region_id"]) == expected_region_id for r in reports)
    else:
        checks["class"] = len(reports) == 0
        checks["region_type"] = len(reports) == 0
        checks["region_id"] = len(reports) == 0
    if case["case_id"].startswith("P1B_") and case["path"] == "HREAD":
        checks["single_latency"] = metrics["report_latencies"][0] == max(1, case["delay"])
        checks["no_drop"] = metrics["dropped_reports"] == 0
        expected_page = encode_address(1, case["bank"], 0, 0) // 4096
        checks["page_region"] = (
            (len(reports) == 1 and reports[0]["region_type"] == case["granularity"]
             and (case["granularity"] == "bank" and int(reports[0]["region_id"]) == case["bank"]
                  or case["granularity"] == "page" and int(reports[0]["region_id"]) == expected_page)))
    return all(checks.values()), checks, metrics


def main() -> int:
    RUNS.mkdir(parents=True, exist_ok=True)
    RESULTS.mkdir(parents=True, exist_ok=True)
    binary_sha = sha256(SIM)
    patch_sha = sha256(ROOT / "environment/source.patch")
    cases = build_cases()
    manifest = {
        "started_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "binary_sha256": binary_sha, "patch_sha256": patch_sha, "cases": [],
    }
    failures = 0
    summary_rows = []
    for case in cases:
        out = case["out"]
        out.mkdir(parents=True, exist_ok=True)
        params = out / "params.ini"
        params.write_text(case_params(case))
        config = {
            "experiment": "G4", "case_id": case["case_id"], "profile": case["profile"],
            "path": case["path"], "fault_kind": case["fault"], "target_bank": case["bank"],
            "target_lane": 0, "data_bit_in_lane": 15, "fault_bit2": 1,
            "granularity": case["granularity"], "transport_delay_cycles": case["delay"],
            "service_interval_cycles": case["interval"], "report_queue_capacity": 64,
            "repeat_reads": case.get("repeat_reads", 32), "watchdog_cycles": 100000,
            "device_ini": str(DEVICE_INI), "system_ini": str(SYSTEM_INI),
            "output_dir": str(out),
        }
        (out / "config.json").write_text(json.dumps(config, indent=2) + "\n")
        env = os.environ.copy()
        env["PHASE1_CONFIG"] = str(params)
        env["PHASE1_OUT"] = str(out)
        start = time.time()
        try:
            proc = subprocess.run([str(SIM), "--gtest_filter=Phase1G4Fixture.RunCase"],
                                  cwd=SIM_DIR, env=env, text=True, capture_output=True,
                                  timeout=120)
            exit_code = proc.returncode
            stdout = proc.stdout
            stderr = proc.stderr
        except subprocess.TimeoutExpired as exc:
            exit_code = 124
            stdout = (exc.stdout or "") if isinstance(exc.stdout, str) else ""
            stderr = (exc.stderr or "") if isinstance(exc.stderr, str) else ""
        elapsed = time.time() - start
        (out / "stdout.log").write_text(stdout)
        (out / "stderr.log").write_text(stderr)
        (out / "exit_code").write_text(f"{exit_code}\n")
        metadata = {
            "schema_version": 1, "run_id": case["case_id"], "experiment": "G4",
            "seed": 0, "upstream_commit": "3703d1f19c8f027360cc33a3243eb271e3bb6898",
            "patch_sha256": patch_sha, "binary_sha256": binary_sha,
            "config_sha256": sha256(params),
            "reference_id": case["case_id"], "environment_hash": json.loads(
                (ROOT / "environment/environment.json").read_text())["environment_hash"],
            "start_time_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "wall_seconds": round(elapsed, 3), "exit_code": exit_code, "run_status": "RUNNING",
        }
        (out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        if exit_code != 0:
            failures += 1
            metadata["run_status"] = "FAIL"
            (out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
            continue
        try:
            passed, checks, metrics = validate_case(case)
        except Exception as exc:  # noqa: BLE001
            passed, checks, metrics = False, {"validation_error": str(exc)}, {}
        metadata["run_status"] = "PASS" if passed else "FAIL"
        (out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        (out / "validation.json").write_text(json.dumps(
            {"case_id": case["case_id"], "pass": passed, "checks": checks,
             "metrics": metrics}, indent=2) + "\n")
        summary_rows.append({
            "case_id": case["case_id"], "family": "P1B" if case["path"].startswith("HREAD_BURST")
            or case["case_id"].startswith("P1B") else "P1A",
            "profile": case["profile"], "path": case["path"], "fault_kind": case["fault"],
            "target_bank": case["bank"], "granularity": case["granularity"],
            "delay": case["delay"], "interval": case["interval"],
            "internal_ce": metrics.get("internal_ce"), "internal_ue": metrics.get("internal_ue"),
            "delivered_reports": metrics.get("delivered_reports"),
            "dropped_reports": metrics.get("dropped_reports"),
            "format_invalid": metrics.get("format_invalid"),
            "output_equals_original": metrics.get("output_equals_original"),
            "report_latencies": json.dumps(metrics.get("report_latencies", [])),
            "pass": passed,
        })
        manifest["cases"].append({"case_id": case["case_id"], "exit_code": exit_code,
                                 "pass": passed})
        if not passed:
            failures += 1
        print(f"{case['case_id']}: exit={exit_code} pass={passed}")
    fields = ["case_id", "family", "profile", "path", "fault_kind", "target_bank",
              "granularity", "delay", "interval", "internal_ce", "internal_ue",
              "delivered_reports", "dropped_reports", "format_invalid",
              "output_equals_original", "report_latencies", "pass"]
    with (RESULTS / "summary.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        writer.writerows(summary_rows)
    p1a = [r for r in summary_rows if r["family"] == "P1A"]
    p1b = [r for r in summary_rows if r["family"] == "P1B"]
    with (RESULTS / "p1a_summary.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        writer.writerows(p1a)
    with (RESULTS / "p1b_summary.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        writer.writerows(p1b)
    checks = {
        "all_cases_present": len(summary_rows) + failures == len(cases),
        "p1a_count": len(p1a) == 144,
        "p1b_count": len(p1b) == 63,
        "all_p1a_pass": all(r["pass"] for r in p1a),
        "all_p1b_pass": all(r["pass"] for r in p1b),
        "no_case_failure": failures == 0,
    }
    result = {"gate": "G4", "pass": all(checks.values()), "checks": checks,
              "binary_sha256": binary_sha, "patch_sha256": patch_sha,
              "case_count": len(cases), "failures": failures}
    (RESULTS / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
    manifest["finished_utc"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    manifest["pass"] = result["pass"]
    (RUNS / "run_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for key, value in checks.items():
        print(("PASS" if value else "FAIL"), key)
    print("G4_RUNNER=" + ("PASS" if result["pass"] else "FAIL"))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
