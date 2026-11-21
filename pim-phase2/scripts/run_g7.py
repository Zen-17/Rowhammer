#!/usr/bin/env python3
"""Run and validate the G7 trusted-input recovery gate.

The runner starts one simulator process per case.  Positive cases use a real
CE report or an explicit RECOVERY_PROBE to request quiesce, discard the old
uncommitted work, reset PIM state, restore A/X through simulated host writes,
re-execute the whole W1 kernel and verify the published output bit-for-bit.
Negative cases must be rejected with one of the five required statuses, and
two deliberately broken recovery controls must be caught.
"""
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
RUNS = ROOT / "runs/g7"
RESULTS = ROOT / "results/g7"
DEVICE_INI = SIM_DIR / "ini/HBM2_samsung_2M_16B_x64.ini"
SYSTEM_1CH = ROOT / "configs/phase1_system_1ch.ini"
UPSTREAM_COMMIT = "3703d1f19c8f027360cc33a3243eb271e3bb6898"
STATUS_CASES = [
    "CHECKPOINT_MISSING",
    "VERSION_MISMATCH",
    "CHECKPOINT_CORRUPT",
    "ALREADY_COMMITTED",
    "STALE_HANDLE",
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def params_text(case: dict) -> str:
    if case["kind"] == "status_negative":
        return (
            "experiment=G7\n"
            f"case={case['status']}\n"
            f"output_dir={case['out']}\n"
        )
    return (
        "experiment=G7\n"
        "scenario=POSITIVE\n"
        f"trigger={case['trigger']}\n"
        f"negative_control={case['negative_control']}\n"
        f"n_tiles={case['n_tiles']}\n"
        f"seed={case['seed']}\n"
        "handler_delay=8\n"
        f"watchdog_cycles={case['watchdog']}\n"
        f"device_ini={DEVICE_INI}\n"
        f"system_ini={SYSTEM_1CH}\n"
        f"output_dir={case['out']}\n"
    )


def build_cases() -> list[dict]:
    cases: list[dict] = []
    for n_tiles in (1, 8):
        for seed in range(10):
            cases.append({
                "kind": "positive",
                "case_id": f"POS_REPORT_nt{n_tiles}_s{seed}",
                "trigger": "REPORT",
                "negative_control": "NONE",
                "n_tiles": n_tiles,
                "seed": seed,
                "watchdog": 400000,
                "filter": "Phase1G7Fixture.RunCase",
                "out": RUNS / "positive" / f"nt{n_tiles}_s{seed}",
            })
    for n_tiles in (1, 8):
        for seed in (0, 1):
            cases.append({
                "kind": "probe",
                "case_id": f"POS_PROBE_nt{n_tiles}_s{seed}",
                "trigger": "RECOVERY_PROBE",
                "negative_control": "NONE",
                "n_tiles": n_tiles,
                "seed": seed,
                "watchdog": 400000,
                "filter": "Phase1G7Fixture.RunCase",
                "out": RUNS / "probe" / f"nt{n_tiles}_s{seed}",
            })
    cases.append({
        "kind": "negative_control",
        "case_id": "NEG_NO_INPUT_RESTORE_nt1_s0",
        "trigger": "REPORT",
        "negative_control": "NO_INPUT_RESTORE",
        "n_tiles": 1,
        "seed": 0,
        "watchdog": 400000,
        "filter": "Phase1G7Fixture.RunCase",
        "out": RUNS / "negative_control" / "NO_INPUT_RESTORE",
    })
    cases.append({
        "kind": "negative_control",
        "case_id": "NEG_NO_STATE_RESET_nt1_s0",
        "trigger": "REPORT",
        "negative_control": "NO_STATE_RESET",
        "n_tiles": 1,
        "seed": 0,
        "watchdog": 10000,
        "filter": "Phase1G7Fixture.RunCase",
        "out": RUNS / "negative_control" / "NO_STATE_RESET",
    })
    for status in STATUS_CASES:
        cases.append({
            "kind": "status_negative",
            "case_id": status,
            "status": status,
            "filter": "Phase1G7NegativeFixture.RunCase",
            "out": RUNS / "negative_statuses" / status,
        })
    return cases


def expected_words(n_tiles: int, seed: int) -> np.ndarray:
    dim = n_tiles * 16 * 1 * 1 * 8
    index = np.arange(dim * 16, dtype=np.int64)
    a = ((index + seed) % 17 - 8).astype(np.float32) / np.float32(8.0)
    x = ((3 * index + seed) % 13 - 6).astype(np.float32) / np.float32(8.0)
    return (a.astype(np.float16) + x.astype(np.float16)).astype(np.float16)


def independent_output_check(case: dict, run_dir: Path) -> bool:
    path = run_dir / "output.bin"
    if not path.exists():
        return False
    dim = case["n_tiles"] * 128
    if path.stat().st_size != dim * 32:
        return False
    observed = np.frombuffer(path.read_bytes(), dtype="<u2")
    expected = expected_words(case["n_tiles"], case["seed"])
    return observed.shape == expected.shape and np.array_equal(
        observed.view(np.uint16), expected.view(np.uint16))


def validate_positive(case: dict, metrics: dict, run_dir: Path) -> tuple[bool, dict]:
    dim = case["n_tiles"] * 128
    checks = {
        "trigger_source": metrics.get("trigger_source") == case["trigger"],
        "old_output_not_published": metrics.get("old_published_before_restore") is False,
        "recovery_state_restoring": metrics.get("recovery_state_restoring") is True,
        "consumers_blocked_during_restore": (
            metrics.get("consumer_blocked_during_restore") is True),
        "version_changed": (
            metrics.get("version_changed") is True
            and metrics.get("new_version", 0) > metrics.get("old_version", 0)),
        "old_handle_stale": metrics.get("old_handle_stale") is True,
        "checkpoint_bytes": metrics.get("checkpoint_bytes") == 2 * dim * 32,
        "restore_writes": metrics.get("restore_writes") == 2 * dim,
        "restore_bytes": metrics.get("restore_bytes") == 2 * dim * 32,
        "restored_logical_inputs": (
            metrics.get("restored_a_ok") is True
            and metrics.get("restored_x_ok") is True),
        "restored_raw_inputs": (
            metrics.get("restored_raw_a_ok") is True
            and metrics.get("restored_raw_x_ok") is True
            and metrics.get("restored_raw_a_mismatches") == 0
            and metrics.get("restored_raw_x_mismatches") == 0),
        "nondefault_state_was_present": metrics.get("nondefault_before_reset") is True,
        "state_reset_ok": (
            metrics.get("reset_ok") is True
            and metrics.get("state_reset_count") == 1
            and metrics.get("mode_after_reset") == "SB"),
        "old_pending_discarded": metrics.get("discarded_pending", 0) > 0,
        "reexec_drained": (
            metrics.get("reexec_pending") == 0
            and metrics.get("reexec_outstanding") == 0),
        "reexec_had_no_reports": (
            metrics.get("reexec_report_count") == 0
            and metrics.get("reexec_ce_count") == 0
            and metrics.get("reexec_ue_count") == 0),
        "new_output_published": (
            metrics.get("new_published") is True
            and metrics.get("new_state") == "PUBLISHED"),
        "host_read_allowed": metrics.get("host_read") is True,
        "raw_b_all_ok": (
            metrics.get("raw_b_all_ok") is True
            and metrics.get("raw_b_mismatches") == 0),
        "bit_exact_metric": metrics.get("output_bit_exact") is True,
        "recovery_cycles_positive": metrics.get("recovery_cycles", 0) > 0,
        "independent_output_bit_exact": independent_output_check(case, run_dir),
    }
    if case["trigger"] == "REPORT":
        checks["report_trigger_seen"] = (
            metrics.get("old_report_count", 0) >= 1
            and metrics.get("old_report_cycle", 0) > 0
            and metrics.get("old_state_before_quiesce") == "SUSPECT")
    else:
        checks["probe_trigger_seen"] = (
            metrics.get("old_report_count") == 0
            and metrics.get("old_state_before_quiesce") == "PRODUCING")
    return all(checks.values()), checks


def validate_negative_control(case: dict, metrics: dict) -> tuple[bool, dict]:
    if case["negative_control"] == "NO_INPUT_RESTORE":
        checks = {
            "restore_skipped": metrics.get("restore_skipped") is True,
            "recovery_state_restoring": metrics.get("recovery_state_restoring") is True,
            "consumers_blocked_during_restore": (
                metrics.get("consumer_blocked_during_restore") is True),
            "no_restore_writes": metrics.get("restore_writes") == 0,
            "raw_input_still_corrupt": metrics.get("restored_raw_a_mismatches", 0) >= 1,
            "output_not_published": (
                metrics.get("new_published") is False
                and metrics.get("host_read") is False),
            "output_not_bit_exact": metrics.get("output_bit_exact") is False,
            "ce_observed_on_reexec": metrics.get("reexec_ce_count", 0) >= 1,
            "negative_caught": metrics.get("caught") is True,
        }
    else:
        checks = {
            "restore_timeout": metrics.get("restore_timeout") is True,
            "restore_incomplete": metrics.get("restore_incomplete") is True,
            "state_reset_skipped": metrics.get("state_reset_count") == 0,
            "stale_mode_present": metrics.get("mode_after_reset") != "SB",
            "output_not_published": metrics.get("new_published") is False,
            "output_not_bit_exact": metrics.get("output_bit_exact") is False,
            "negative_caught": metrics.get("caught") is True,
        }
    return all(checks.values()), checks


def validate_status_negative(case: dict, metrics: dict) -> tuple[bool, dict]:
    checks = {
        "test_passed": metrics.get("pass") is True,
        "observed_expected_status": metrics.get("observed_status") == case["status"],
    }
    return all(checks.values()), checks


def main() -> int:
    RUNS.mkdir(parents=True, exist_ok=True)
    RESULTS.mkdir(parents=True, exist_ok=True)
    if not SIM.exists():
        print(f"missing simulator binary: {SIM}", file=sys.stderr)
        return 2
    binary_sha = sha256(SIM)
    patch_path = ROOT / "environment/source.patch"
    patch_sha = sha256(patch_path) if patch_path.exists() else "MISSING"
    environment_hash = json.loads(
        (ROOT / "environment/environment.json").read_text())["environment_hash"]

    cases = build_cases()
    manifest = {
        "gate": "G7",
        "started_utc": utc_now(),
        "upstream_commit": UPSTREAM_COMMIT,
        "binary_sha256": binary_sha,
        "patch_sha256": patch_sha,
        "environment_hash": environment_hash,
        "cases": [],
    }
    rows: list[dict] = []
    failures = 0
    observed_statuses: set[str] = set()

    for case in cases:
        out: Path = case["out"]
        out.mkdir(parents=True, exist_ok=True)
        params = out / "params.ini"
        params.write_text(params_text(case))
        (out / "config.json").write_text(json.dumps(
            {k: str(v) for k, v in case.items() if k != "out"}, indent=2) + "\n")

        env = os.environ.copy()
        env["PHASE1_CONFIG"] = str(params)
        env["PHASE1_OUT"] = str(out)
        metadata = {
            "schema_version": 1,
            "run_id": case["case_id"],
            "gate": "G7",
            "kind": case["kind"],
            "upstream_commit": UPSTREAM_COMMIT,
            "patch_sha256": patch_sha,
            "binary_sha256": binary_sha,
            "config_sha256": sha256(params),
            "reference_id": case["case_id"],
            "environment_hash": environment_hash,
            "start_time_utc": utc_now(),
            "run_status": "RUNNING",
        }
        for key in ("n_tiles", "seed", "trigger", "negative_control", "status"):
            if key in case:
                metadata[key] = case[key]

        started = time.time()
        try:
            proc = subprocess.run(
                [str(SIM), f"--gtest_filter={case['filter']}"],
                cwd=SIM_DIR,
                env=env,
                text=True,
                capture_output=True,
                timeout=180,
            )
            exit_code = proc.returncode
            stdout = proc.stdout
            stderr = proc.stderr
        except subprocess.TimeoutExpired as exc:
            exit_code = 124
            stdout = exc.stdout if isinstance(exc.stdout, str) else ""
            stderr = exc.stderr if isinstance(exc.stderr, str) else ""
        elapsed = time.time() - started
        (out / "stdout.log").write_text(stdout)
        (out / "stderr.log").write_text(stderr)
        (out / "exit_code").write_text(f"{exit_code}\n")
        metadata["wall_seconds"] = round(elapsed, 3)
        metadata["exit_code"] = exit_code

        metrics: dict = {}
        passed = False
        checks: dict = {"exit_code_zero": exit_code == 0}
        error = ""
        if exit_code != 0:
            error = f"simulator exit {exit_code}"
        else:
            try:
                metrics = json.loads((out / "metrics.json").read_text())
                if case["kind"] in ("positive", "probe"):
                    passed, checks = validate_positive(case, metrics, out)
                elif case["kind"] == "negative_control":
                    passed, checks = validate_negative_control(case, metrics)
                else:
                    passed, checks = validate_status_negative(case, metrics)
                    observed_statuses.add(metrics.get("observed_status", "UNKNOWN"))
            except Exception as exc:  # noqa: BLE001
                error = str(exc)
                passed = False
        if not passed:
            failures += 1

        expected_negative = case["kind"] in ("negative_control", "status_negative")
        if expected_negative:
            metadata["run_status"] = (
                "EXPECTED_NEGATIVE_PASS" if passed else "EXPECTED_NEGATIVE_FAIL")
        else:
            metadata["run_status"] = "PASS" if passed else "FAIL"
        (out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        (out / "validation.json").write_text(json.dumps({
            "case_id": case["case_id"],
            "kind": case["kind"],
            "expected_negative": expected_negative,
            "pass": passed,
            "checks": checks,
            "error": error,
        }, indent=2) + "\n")

        row = {
            "case_id": case["case_id"],
            "kind": case["kind"],
            "n_tiles": case.get("n_tiles", ""),
            "seed": case.get("seed", ""),
            "trigger": case.get("trigger", ""),
            "negative_control": case.get("negative_control", ""),
            "exit_code": exit_code,
            "expected_negative": expected_negative,
            "pass": passed,
            "old_version": metrics.get("old_version", ""),
            "new_version": metrics.get("new_version", ""),
            "checkpoint_bytes": metrics.get("checkpoint_bytes", ""),
            "restore_writes": metrics.get("restore_writes", ""),
            "restore_bytes": metrics.get("restore_bytes", ""),
            "discarded_pending": metrics.get("discarded_pending", ""),
            "old_report_cycle": metrics.get("old_report_cycle", ""),
            "ack_cycle": metrics.get("ack_cycle", ""),
            "restore_cycles": metrics.get("restore_cycles", ""),
            "reexec_cycles": metrics.get("reexec_cycles", ""),
            "recovery_cycles": metrics.get("recovery_cycles", ""),
            "reexec_ce_count": metrics.get("reexec_ce_count", ""),
            "state_reset_count": metrics.get("state_reset_count", ""),
            "mode_before_reset": metrics.get("mode_before_reset", ""),
            "mode_after_reset": metrics.get("mode_after_reset", ""),
            "new_state": metrics.get("new_state", ""),
            "output_bit_exact": metrics.get("output_bit_exact", ""),
            "observed_status": metrics.get("observed_status", ""),
        }
        rows.append(row)
        manifest["cases"].append({
            "case_id": case["case_id"],
            "kind": case["kind"],
            "exit_code": exit_code,
            "pass": passed,
            "expected_negative": expected_negative,
        })
        print(f"{case['case_id']}: exit={exit_code} pass={passed}")

    positive_rows = [r for r in rows if r["kind"] in ("positive", "probe")]
    probe_rows = [r for r in rows if r["kind"] == "probe"]
    negative_control_rows = [r for r in rows if r["kind"] == "negative_control"]
    status_rows = [r for r in rows if r["kind"] == "status_negative"]

    checks = {
        "positive_case_count": len([r for r in rows if r["kind"] == "positive"]) == 20,
        "probe_case_count": len(probe_rows) == 4,
        "negative_control_count": len(negative_control_rows) == 2,
        "status_negative_count": len(status_rows) == 5,
        "all_cases_pass": failures == 0,
        "all_positive_outputs_bit_exact": all(
            r["output_bit_exact"] is True for r in positive_rows),
        "all_negative_controls_caught": all(r["pass"] for r in negative_control_rows),
        "all_status_negatives_pass": all(r["pass"] for r in status_rows),
        "all_five_statuses_observed": set(STATUS_CASES).issubset(observed_statuses),
        "all_positive_versions_changed": all(
            r["old_version"] != "" and r["new_version"] != ""
            and int(r["new_version"]) > int(r["old_version"]) for r in positive_rows),
        "all_restores_used_expected_bytes": all(
            r["restore_bytes"] == int(r["n_tiles"]) * 2 * 128 * 32 for r in positive_rows),
        "report_and_probe_triggers_labeled": (
            any(r["trigger"] == "REPORT" for r in positive_rows)
            and any(r["trigger"] == "RECOVERY_PROBE" for r in positive_rows)),
        "no_simulation_failures": all(r["exit_code"] == 0 for r in rows),
    }

    fields = list(rows[0].keys()) if rows else []
    with (RESULTS / "summary.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    result = {
        "gate": "G7",
        "pass": all(checks.values()),
        "checks": checks,
        "binary_sha256": binary_sha,
        "patch_sha256": patch_sha,
        "case_count": len(rows),
        "failures": failures,
        "observed_statuses": sorted(observed_statuses),
    }
    (RESULTS / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
    manifest["finished_utc"] = utc_now()
    manifest["pass"] = result["pass"]
    manifest["case_count"] = len(rows)
    (RUNS / "run_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    for key, value in checks.items():
        print(("PASS" if value else "FAIL"), key)
    print("G7_RUNNER=" + ("PASS" if result["pass"] else "FAIL"))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
