#!/usr/bin/env python3
"""Run G5 output-state/host-device gate smoke and validate evidence."""
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

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
SIM_DIR = ROOT / "simulator"
SIM = SIM_DIR / "sim"
RUNS = ROOT / "runs/g5"
RESULTS = ROOT / "results/g5"
DEVICE_INI = SIM_DIR / "ini/HBM2_samsung_2M_16B_x64.ini"
SYSTEM_INI = ROOT / "configs/phase1_system_1ch.ini"
REPORT_HEADER = ["report_seq", "report_cycle", "error_class", "region_type", "region_id"]
TRUTH_HEADER = ["record_type", "detection_id", "fault_id", "fault_cycle", "storage_key",
                "codeword_index", "bit_positions", "raw_before", "raw_after",
                "target_access_id", "detect_cycle", "decoder_status", "eligible_report_id", "dropped"]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def params_text(case: dict) -> str:
    return f"""experiment=G5
profile=LOCAL_VISIBLE
fault_kind={case['fault']}
n_tiles={case['n_tiles']}
seed=0
hold_cycles={case['hold']}
consumer={case['consumer']}
transport_delay_cycles=0
service_interval_cycles=1
watchdog_cycles=100000
device_ini={DEVICE_INI}
system_ini={SYSTEM_INI}
output_dir={case['out']}
"""


def reference_b(n_tiles: int):
    dim = n_tiles * 16 * 1 * 1 * 8
    index = np.arange(dim * 16, dtype=np.int64)
    a = (((index % 17 - 8).astype(np.float32) / np.float32(8.0))).astype(np.float16)
    x = (((3 * index % 13 - 6).astype(np.float32) / np.float32(8.0))).astype(np.float16)
    return (a + x).astype(np.float16)


def build_cases() -> list[dict]:
    cases = []
    for fault in ("F0", "F1", "F2"):
        for hold in (0, 8):
            for consumer in ("HOST", "PIM"):
                out = RUNS / f"{fault}_h{hold}_{consumer.lower()}"
                cases.append({
                    "case_id": f"{fault}_h{hold}_{consumer}",
                    "fault": fault, "hold": hold, "consumer": consumer,
                    "n_tiles": 1, "out": out,
                })
    return cases


def validate_case(case: dict) -> tuple[bool, dict, dict]:
    run = case["out"]
    metrics = json.loads((run / "metrics.json").read_text())
    with (run / "reports.csv").open(newline="") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames != REPORT_HEADER:
            raise ValueError(f"report header mismatch: {reader.fieldnames}")
        reports = list(reader)
    with (run / "truth.csv").open(newline="") as fh:
        truth_reader = csv.DictReader(fh)
        if truth_reader.fieldnames != TRUTH_HEADER:
            raise ValueError(f"truth header mismatch: {truth_reader.fieldnames}")
        truth = list(truth_reader)
    checks = {}
    if case["fault"] == "F0":
        checks["truth_empty"] = len(truth) == 0
    else:
        checks["truth_injection"] = len(truth) == 1 and truth[0]["raw_before"] != truth[0]["raw_after"]
    checks["no_timeout"] = metrics["timeout"] is False
    state = metrics["state_after_hold"]
    if case["fault"] == "F0":
        checks["published"] = metrics["published"] is True and state == "PUBLISHED"
        checks["admitted"] = metrics["host_admitted"] is True and metrics["pim_admitted"] is True
        checks["not_blocked"] = metrics["host_blocked"] is False and metrics["pim_blocked"] is False
        checks["no_reports"] = metrics["internal_detections"] == 0 and len(reports) == 0
        checks["bypass_not_triggered"] = metrics["bypass_assert"] is False
        if case["consumer"] == "HOST":
            expected = reference_b(case["n_tiles"])
            actual = np.frombuffer((run / "output.bin").read_bytes(), dtype="<u2")
            checks["output_bit_exact"] = (
                actual.size == expected.size
                and np.array_equal(actual.view(np.uint16), expected.view(np.uint16)))
        else:
            checks["output_empty_for_pim"] = (run / "output.bin").stat().st_size == 0
    elif case["fault"] == "F1":
        checks["suspect"] = state == "SUSPECT" and metrics["published"] is False
        checks["blocked"] = metrics["host_blocked"] is True and metrics["pim_blocked"] is True
        checks["bypass_assert"] = metrics["bypass_assert"] is True
        checks["ce"] = metrics["internal_ce"] == 1 and metrics["delivered_reports"] == 1
        checks["report_class"] = len(reports) == 1 and reports[0]["error_class"] == "CE"
        checks["output_empty"] = (run / "output.bin").stat().st_size == 0
    else:
        checks["poisoned"] = state == "POISONED" and metrics["published"] is False
        checks["blocked"] = metrics["host_blocked"] is True and metrics["pim_blocked"] is True
        checks["bypass_assert"] = metrics["bypass_assert"] is True
        checks["ue"] = metrics["internal_ue"] == 1 and metrics["delivered_reports"] == 1
        checks["report_class"] = len(reports) == 1 and reports[0]["error_class"] == "UE"
        checks["output_empty"] = (run / "output.bin").stat().st_size == 0
    return all(checks.values()), checks, metrics


def main() -> int:
    RUNS.mkdir(parents=True, exist_ok=True)
    RESULTS.mkdir(parents=True, exist_ok=True)
    binary_sha = sha256(SIM)
    patch_sha = sha256(ROOT / "environment/source.patch")
    manifest = {"started_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                "binary_sha256": binary_sha, "patch_sha256": patch_sha, "cases": []}
    failures = 0
    rows = []
    for case in build_cases():
        out = case["out"]
        out.mkdir(parents=True, exist_ok=True)
        params = out / "params.ini"
        params.write_text(params_text(case))
        (out / "config.json").write_text(json.dumps({k: str(v) for k, v in case.items()
                                                      if k != "out"}, indent=2) + "\n")
        env = os.environ.copy()
        env["PHASE1_CONFIG"] = str(params)
        env["PHASE1_OUT"] = str(out)
        start = time.time()
        try:
            proc = subprocess.run([str(SIM), "--gtest_filter=Phase1G5Fixture.RunCase"],
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
            "schema_version": 1, "run_id": case["case_id"], "experiment": "G5",
            "seed": 0, "upstream_commit": "3703d1f19c8f027360cc33a3243eb271e3bb6898",
            "patch_sha256": patch_sha, "binary_sha256": binary_sha,
            "config_sha256": sha256(params), "reference_id": case["case_id"],
            "environment_hash": json.loads(
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
        outcome = ("POISONED" if case["fault"] == "F2" else
                   "TIMELY_CONTROL" if case["fault"] == "F1" else
                   "NO_REPORT_PUBLISHED")
        (out / "validation.json").write_text(json.dumps(
            {"case_id": case["case_id"], "pass": passed, "checks": checks,
             "outcome_class": outcome, "metrics": metrics}, indent=2) + "\n")
        rows.append({
            "case_id": case["case_id"], "fault_kind": case["fault"],
            "hold_cycles": case["hold"], "consumer": case["consumer"],
            "state": metrics.get("state_after_hold"),
            "published": metrics.get("published"),
            "host_admitted": metrics.get("host_admitted"),
            "pim_admitted": metrics.get("pim_admitted"),
            "host_blocked": metrics.get("host_blocked"),
            "pim_blocked": metrics.get("pim_blocked"),
            "bypass_assert": metrics.get("bypass_assert"),
            "internal_ce": metrics.get("internal_ce"),
            "internal_ue": metrics.get("internal_ue"),
            "delivered_reports": metrics.get("delivered_reports"),
            "late_report": metrics.get("late_report"),
            "outcome_class": outcome,
            "pass": passed,
        })
        manifest["cases"].append({"case_id": case["case_id"], "exit_code": exit_code,
                                 "pass": passed, "outcome_class": outcome})
        if not passed:
            failures += 1
        print(f"{case['case_id']}: exit={exit_code} pass={passed}")

    runtime_tests = subprocess.run(
        [str(SIM), "--gtest_filter=Phase1Runtime.*"],
        cwd=SIM_DIR, text=True, capture_output=True, timeout=60)
    (RUNS / "runtime_tests.log").write_text(runtime_tests.stdout + runtime_tests.stderr)
    checks = {
        "case_count": len(rows) == 12,
        "all_cases_pass": all(r["pass"] for r in rows),
        "no_simulation_failures": failures == 0,
        "runtime_same_cycle_test": runtime_tests.returncode == 0,
        "all_f0_published": all(r["published"] is True for r in rows if r["fault_kind"] == "F0"),
        "all_f1_suspect": all(r["state"] == "SUSPECT" for r in rows if r["fault_kind"] == "F1"),
        "all_f2_poisoned": all(r["state"] == "POISONED" for r in rows if r["fault_kind"] == "F2"),
        "all_blocked_cases_blocked": all(
            r["host_blocked"] is True and r["pim_blocked"] is True for r in rows
            if r["fault_kind"] in ("F1", "F2")),
        "bypass_asserted": all(r["bypass_assert"] is True for r in rows
                               if r["fault_kind"] in ("F1", "F2")),
    }
    fields = ["case_id", "fault_kind", "hold_cycles", "consumer", "state", "published",
              "host_admitted", "pim_admitted", "host_blocked", "pim_blocked",
              "bypass_assert", "internal_ce", "internal_ue", "delivered_reports",
              "late_report", "outcome_class", "pass"]
    with (RESULTS / "summary.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    result = {"gate": "G5", "pass": all(checks.values()), "checks": checks,
              "binary_sha256": binary_sha, "patch_sha256": patch_sha,
              "case_count": len(rows), "failures": failures}
    (RESULTS / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
    manifest["finished_utc"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    manifest["pass"] = result["pass"]
    (RUNS / "run_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for key, value in checks.items():
        print(("PASS" if value else "FAIL"), key)
    print("G5_RUNNER=" + ("PASS" if result["pass"] else "FAIL"))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
