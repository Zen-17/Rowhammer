#!/usr/bin/env python3
"""Run G6 pause/quiesce smoke and validate post-ACK data quiescence."""
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
RUNS = ROOT / "runs/g6"
RESULTS = ROOT / "results/g6"
DEVICE_INI = SIM_DIR / "ini/HBM2_samsung_2M_16B_x64.ini"
SYSTEM_1CH = ROOT / "configs/phase1_system_1ch.ini"
SYSTEM_2CH = ROOT / "configs/phase1_system_2ch.ini"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def params_text(case: dict) -> str:
    return f"""experiment=G6
scenario={case['scenario']}
channels={case['channels']}
capacity_mb={case['capacity_mb']}
feeder_limit={case['feeder_limit']}
handler_delay={case['handler_delay']}
observation_cycles=8000
n_tiles={case['n_tiles']}
watchdog_cycles=200000
device_ini={DEVICE_INI}
system_ini={case['system_ini']}
output_dir={case['out']}
"""


def build_cases() -> list[dict]:
    cases = []
    for scenario in ("SYNTHETIC_BEFORE", "SYNTHETIC_QUEUE", "SYNTHETIC_INFLIGHT",
                     "SYNTHETIC_NEAR"):
        for limit in (1, 8, 32):
            out = RUNS / "synthetic" / f"{scenario}_K{limit}"
            cases.append({
                "case_id": f"{scenario}_K{limit}", "scenario": scenario, "channels": 1,
                "capacity_mb": 256, "feeder_limit": limit, "handler_delay": 0,
                "n_tiles": 8, "system_ini": str(SYSTEM_1CH), "out": out,
            })
    for delay in (0, 8, 64):
        out = RUNS / "actual_ce" / f"ACTUAL_CE_L{delay}"
        cases.append({
            "case_id": f"ACTUAL_CE_L{delay}", "scenario": "ACTUAL_CE", "channels": 1,
            "capacity_mb": 256, "feeder_limit": 8, "handler_delay": delay,
            "n_tiles": 1, "system_ini": str(SYSTEM_1CH), "out": out,
        })
    out = RUNS / "two_channel" / "TWO_CHANNEL"
    cases.append({
        "case_id": "TWO_CHANNEL", "scenario": "TWO_CHANNEL", "channels": 2,
        "capacity_mb": 512, "feeder_limit": 8, "handler_delay": 0,
        "n_tiles": 8, "system_ini": str(SYSTEM_2CH), "out": out,
    })
    return cases


def validate_case(case: dict) -> tuple[bool, dict, dict]:
    run = case["out"]
    metrics = json.loads((run / "metrics.json").read_text())
    checks = {
        "requested": metrics["requested"] is True,
        "acked": metrics["acked"] is True,
        "no_timeout": metrics["timeout"] is False,
        "ack_after_request": metrics["ack_cycle"] >= metrics["request_cycle"],
        "data_stopped_after_ack": metrics["data_delta_after_ack"] == 0,
        "clock_advanced": metrics["clock_delta"] > 0,
        "refresh_not_regressed": metrics["refresh_after"] >= metrics["refresh_before"],
    }
    if case["scenario"].startswith("SYNTHETIC"):
        checks["external_pending_frozen"] = metrics["external_pending_frozen"] > 0
    if case["scenario"] == "ACTUAL_CE":
        checks["report_seen"] = metrics["report_count"] >= 1 and metrics["report_cycle"] > 0
        checks["request_after_handler_delay"] = (
            metrics["request_cycle"] >= metrics["report_cycle"] + case["handler_delay"])
    if case["scenario"] == "TWO_CHANNEL":
        checks["channel0_stopped"] = metrics["data_delta_after_ack"] == 0
        checks["channel1_progress"] = metrics["other_channel_data_delta"] > 0
        checks["channel0_external_frozen"] = metrics["external_pending_frozen"] > 0
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
            proc = subprocess.run([str(SIM), "--gtest_filter=Phase1G6Fixture.RunCase"],
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
            "schema_version": 1, "run_id": case["case_id"], "experiment": "G6",
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
        (out / "validation.json").write_text(json.dumps(
            {"case_id": case["case_id"], "pass": passed, "checks": checks}, indent=2) + "\n")
        rows.append({
            "case_id": case["case_id"], "scenario": case["scenario"],
            "channels": case["channels"], "feeder_limit": case["feeder_limit"],
            "handler_delay": case["handler_delay"],
            "request_cycle": metrics.get("request_cycle"),
            "ack_cycle": metrics.get("ack_cycle"),
            "quiesce_cycles": metrics.get("quiesce_cycles"),
            "external_pending": metrics.get("external_pending_frozen"),
            "data_delta_after_ack": metrics.get("data_delta_after_ack"),
            "refresh_before": metrics.get("refresh_before"),
            "refresh_after": metrics.get("refresh_after"),
            "clock_delta": metrics.get("clock_delta"),
            "other_channel_delta": metrics.get("other_channel_data_delta"),
            "report_count": metrics.get("report_count"),
            "pass": passed,
        })
        manifest["cases"].append({"case_id": case["case_id"], "exit_code": exit_code,
                                 "pass": passed})
        if not passed:
            failures += 1
        print(f"{case['case_id']}: exit={exit_code} pass={passed}")

    checks = {
        "case_count": len(rows) == 16,
        "all_cases_pass": all(r["pass"] for r in rows),
        "no_simulation_failures": failures == 0,
        "all_acked": all(r["ack_cycle"] is not None for r in rows),
        "all_data_stopped": all(r["data_delta_after_ack"] == 0 for r in rows),
        "all_clock_advanced": all(r["clock_delta"] > 0 for r in rows),
        "all_refresh_not_regressed": all(
            r["refresh_after"] >= r["refresh_before"] for r in rows),
        "synthetic_pending_frozen": all(
            r["external_pending"] > 0 for r in rows if r["scenario"].startswith("SYNTHETIC")),
        "two_channel_progress": next(
            (r["other_channel_delta"] > 0 for r in rows if r["case_id"] == "TWO_CHANNEL"),
            False),
    }
    fields = ["case_id", "scenario", "channels", "feeder_limit", "handler_delay",
              "request_cycle", "ack_cycle", "quiesce_cycles", "external_pending",
              "data_delta_after_ack", "refresh_before", "refresh_after", "clock_delta",
              "other_channel_delta", "report_count", "pass"]
    with (RESULTS / "summary.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    result = {"gate": "G6", "pass": all(checks.values()), "checks": checks,
              "binary_sha256": binary_sha, "patch_sha256": patch_sha,
              "case_count": len(rows), "failures": failures}
    (RESULTS / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
    manifest["finished_utc"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    manifest["pass"] = result["pass"]
    (RUNS / "run_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for key, value in checks.items():
        print(("PASS" if value else "FAIL"), key)
    print("G6_RUNNER=" + ("PASS" if result["pass"] else "FAIL"))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
