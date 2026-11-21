#!/usr/bin/env python3
"""Run the Phase 2 L3 manifest and reconcile every E2E case."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SIM_DIR = ROOT / "simulator"
SIM = SIM_DIR / "sim"
DEFAULT_MANIFEST = ROOT / "configs/generated/phase2/l3-manifest.json"
DEFAULT_RUNS = ROOT / "runs/phase2/l3"
DEFAULT_RESULTS = ROOT / "results/phase2/l3"


def read_json(path: Path) -> dict:
    return json.loads(path.read_text())


def truthy(value) -> bool:
    return value is True or value == 1 or value == "1" or value == "true"


def decision_affected(run_dir: Path) -> list[int]:
    path = run_dir / "decision.json"
    if not path.exists():
        return []
    return [int(v) for v in read_json(path).get("affected", [])]


def validate_case(case: dict, run_dir: Path, metrics: dict) -> tuple[bool, dict]:
    kind = case["expect"]["kind"]
    checks: dict = {"no_timeout": metrics.get("timeout") is False}
    if kind == "e2e0":
        n_tiles = int(case["expect"]["n_tiles"])
        workload = case["expect"]["workload"]
        kernels = 3 if workload == "W2" else 1
        checks.update({
            "published": truthy(metrics.get("published")),
            "output_bit_exact": truthy(metrics.get("output_bit_exact")),
            "reports_during_recovery": metrics.get("reports_during_recovery") == 0,
        })
        if case["expect"]["accounting"] == "ON":
            checks["ledger_entries"] = metrics.get("ledger_entries") == n_tiles * kernels
        else:
            checks["ledger_entries"] = metrics.get("ledger_entries") == 0
    elif kind == "e2e1":
        tile = int(case["expect"]["fault_tile"])
        checks.update({
            "status": metrics.get("observed_status") == "SELECTIVE_RECOVERED",
            "affected_exact": decision_affected(run_dir) == [tile + 1],
            "restore_bytes": metrics.get("restore_bytes") == 4096,
            "published": truthy(metrics.get("published")),
            "output_bit_exact": truthy(metrics.get("output_bit_exact")),
            "reports_during_recovery": metrics.get("reports_during_recovery") == 0,
        })
    elif kind == "e2e1_no_replay":
        tile = int(case["expect"]["fault_tile"])
        checks.update({
            "published": truthy(metrics.get("published")),
            "output_not_exact": not truthy(metrics.get("output_bit_exact")),
            "single_tile_mismatch": metrics.get("mismatch_tiles") == 1,
            "mismatch_tile": str(metrics.get("mismatch_tile_ids")) == str(tile),
        })
    elif kind == "e2e2":
        n_tiles = int(case["expect"]["n_tiles"])
        checks.update({
            "status": metrics.get("observed_status") == "WHOLE_JOB_FALLBACK",
            "output_bit_exact": truthy(metrics.get("output_bit_exact")),
            "replay_tiles": metrics.get("replay_tiles") == n_tiles,
            "restore_bytes": metrics.get("restore_bytes") == 2 * n_tiles * 4096,
            "published": truthy(metrics.get("published")),
        })
    elif kind == "e2e3":
        tile = int(case["expect"]["fault_tile"])
        checks.update({
            "status": metrics.get("observed_status") == "SELECTIVE_RECOVERED",
            "affected_contains": (tile + 1) in decision_affected(run_dir),
            "output_bit_exact": truthy(metrics.get("output_bit_exact")),
            "published": truthy(metrics.get("published")),
        })
    elif kind == "e2e4":
        checks.update({
            "status": metrics.get("observed_status") == "SOURCE_REPAIRED_ONLY",
            "no_replay": metrics.get("replay_tiles") == 0,
            "restore_bytes": metrics.get("restore_bytes") == 4096,
            "output_bit_exact": truthy(metrics.get("output_bit_exact")),
            "post_recovery_ce_zero": metrics.get("post_recovery_ce_count") == 0,
        })
    elif kind == "e2e5":
        tile = int(case["expect"]["fault_tile"])
        affected = decision_affected(run_dir)
        checks.update({
            "status_not_untrusted": metrics.get("observed_status")
            != "OUTSIDE_WINDOW_NO_TRUSTED_INPUT",
            "add_in_affected": (tile + 1) in affected,
            "mul_in_affected": (8 + tile + 1) in affected,
            "output_bit_exact": truthy(metrics.get("output_bit_exact")),
            "published": truthy(metrics.get("published")),
        })
    elif kind == "e2e5_no_replay":
        checks.update({
            "output_not_exact": not truthy(metrics.get("output_bit_exact")),
            "published": truthy(metrics.get("published")),
        })
    elif kind == "e2e6":
        checks.update({
            "reports_during_recovery": metrics.get("reports_during_recovery", 0) >= 1,
            "not_published": not truthy(metrics.get("published")),
            "recovery_failed": truthy(metrics.get("recovery_failed")),
        })
    elif kind == "e2e7":
        tile = int(case["expect"]["fault_tile"])
        checks.update({
            "status": metrics.get("observed_status")
            == "OUTSIDE_WINDOW_NO_TRUSTED_INPUT",
            "no_replay": metrics.get("replay_tiles") == 0,
            "host_read_denied": truthy(metrics.get("host_read_denied")),
            "not_published": not truthy(metrics.get("published")),
            "poisoned_tile": str(tile) in str(metrics.get("poisoned_tiles", "")).split(";"),
        })
    elif kind == "sweep":
        checks.update({
            "published": truthy(metrics.get("published")),
            "output_bit_exact": truthy(metrics.get("output_bit_exact")),
            "reports_during_recovery": metrics.get("reports_during_recovery") == 0,
            "not_untrusted": metrics.get("observed_status")
            != "OUTSIDE_WINDOW_NO_TRUSTED_INPUT",
        })
    else:
        raise ValueError(kind)
    return all(checks.values()), checks


def run_one(case: dict, resume: bool) -> dict:
    run_dir = Path(case["params"]["output_dir"])
    run_dir.mkdir(parents=True, exist_ok=True)
    params_path = run_dir / "params.ini"
    params_text = "".join(f"{key}={value}\n" for key, value in case["params"].items())
    params_path.write_text(params_text)
    (run_dir / "config.json").write_text(json.dumps(case, indent=2) + "\n")

    if resume and (run_dir / "exit_code").exists():
        try:
            exit_code = int((run_dir / "exit_code").read_text().strip())
            validation = read_json(run_dir / "validation.json")
            if exit_code == 0 and validation.get("pass") is True:
                return {"case": case, "status": validation["status"], "pass": True,
                        "metrics": read_json(run_dir / "metrics.json"),
                        "wall_seconds": 0.0, "exit_code": 0, "skipped": True}
        except Exception:  # noqa: BLE001
            pass

    env = os.environ.copy()
    env["PHASE2_CONFIG"] = str(params_path)
    env["PHASE2_OUT"] = str(run_dir)
    started = time.time()
    try:
        proc = subprocess.run([str(SIM), f"--gtest_filter={case['fixture']}"],
                              cwd=SIM_DIR, env=env, text=True, capture_output=True,
                              timeout=300)
        exit_code = proc.returncode
        stdout, stderr = proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as exc:
        exit_code = 124
        stdout = exc.stdout if isinstance(exc.stdout, str) else ""
        stderr = exc.stderr if isinstance(exc.stderr, str) else ""
    elapsed = time.time() - started
    (run_dir / "stdout.log").write_text(stdout)
    (run_dir / "stderr.log").write_text(stderr)
    (run_dir / "exit_code").write_text(f"{exit_code}\n")

    metrics = read_json(run_dir / "metrics.json") if (run_dir / "metrics.json").exists() else {}
    passed = False
    checks: dict = {}
    error = ""
    if exit_code == 124:
        status = "TIMEOUT"
    elif exit_code != 0 or not metrics:
        status = "MISSING" if not metrics else "FAIL"
        error = f"simulator exit {exit_code}"
    else:
        try:
            passed, checks = validate_case(case, run_dir, metrics)
            status = ("EXPECTED_NEGATIVE" if passed and case["expected_negative"]
                      else ("SUCCESS" if passed else "FAIL"))
        except Exception as exc:  # noqa: BLE001
            error = str(exc)
            status = "FAIL"
    validation = {
        "run_id": case["run_id"],
        "group": case["group"],
        "expected_negative": case["expected_negative"],
        "pass": passed,
        "status": status,
        "checks": checks,
        "error": error,
    }
    (run_dir / "validation.json").write_text(json.dumps(validation, indent=2) + "\n")
    metadata = {
        "run_id": case["run_id"],
        "group": case["group"],
        "fixture": case["fixture"],
        "start_time_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "exit_code": exit_code,
        "run_status": status,
        "wall_seconds": round(elapsed, 3),
        "binary_sha256": hashlib.sha256(SIM.read_bytes()).hexdigest(),
    }
    (run_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return {"case": case, "status": status, "pass": passed, "metrics": metrics,
            "wall_seconds": elapsed, "exit_code": exit_code, "skipped": False}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--results-dir", default=str(DEFAULT_RESULTS))
    args = parser.parse_args()
    manifest = read_json(Path(args.manifest))
    cases = manifest["cases"]
    if args.limit:
        cases = cases[:args.limit]
    results: list[dict] = []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as executor:
        futures = {executor.submit(run_one, case, args.resume): case for case in cases}
        for index, future in enumerate(as_completed(futures), start=1):
            result = future.result()
            results.append(result)
            if index % 50 == 0 or index == len(futures):
                print(f"[{index}/{len(futures)}] {result['case']['run_id']} "
                      f"{result['status']}")
    results.sort(key=lambda item: item["case"]["run_id"])
    counts: dict[str, int] = {}
    for result in results:
        counts[result["status"]] = counts.get(result["status"], 0) + 1
    rows = []
    for result in results:
        case = result["case"]
        metrics = result["metrics"]
        row = {
            "run_id": case["run_id"],
            "group": case["group"],
            "status": result["status"],
            "pass": result["pass"],
            "expected_negative": case["expected_negative"],
            "workload": case["params"]["workload"],
            "n_tiles": case["params"]["n_tiles"],
            "granularity": case["params"]["granularity"],
            "fault_kind": case["params"]["fault_kind"],
            "policy": case["params"]["policy"],
            "fault_tile": case["params"]["fault_tile"],
            "seed": case["params"]["seed"],
            "observed_status": metrics.get("observed_status", ""),
            "replay_tiles": metrics.get("replay_tiles", ""),
            "restore_bytes": metrics.get("restore_bytes", ""),
            "recovery_cycles": metrics.get("recovery_cycles", ""),
            "end_to_end_cycles": metrics.get("end_to_end_cycles", ""),
            "reports_during_recovery": metrics.get("reports_during_recovery", ""),
            "output_bit_exact": metrics.get("output_bit_exact", ""),
            "published": metrics.get("published", ""),
            "wall_seconds": round(result["wall_seconds"], 3),
            "exit_code": result["exit_code"],
        }
        rows.append(row)
    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)
    fieldnames = list(rows[0].keys()) if rows else []
    for name in ("run_metrics.csv", "failure_cases.csv"):
        with (results_dir / name).open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fieldnames)
            writer.writeheader()
            if name == "failure_cases.csv":
                writer.writerows([row for row in rows if row["status"] in ("FAIL", "TIMEOUT",
                                                                           "MISSING")])
            else:
                writer.writerows(rows)
    summary_rows = []
    for group in sorted({row["group"] for row in rows}):
        group_rows = [row for row in rows if row["group"] == group]
        summary_rows.append({
            "group": group,
            "cases": len(group_rows),
            "success": sum(1 for row in group_rows if row["status"] == "SUCCESS"),
            "expected_negative": sum(1 for row in group_rows
                                     if row["status"] == "EXPECTED_NEGATIVE"),
            "fail": sum(1 for row in group_rows if row["status"] == "FAIL"),
            "timeout": sum(1 for row in group_rows if row["status"] == "TIMEOUT"),
            "missing": sum(1 for row in group_rows if row["status"] == "MISSING"),
        })
    with (results_dir / "summary.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summary_rows[0].keys()))
        writer.writeheader()
        writer.writerows(summary_rows)
    payload = {
        "cases": len(results),
        "counts": counts,
        "pass": counts.get("FAIL", 0) == 0 and counts.get("TIMEOUT", 0) == 0
        and counts.get("MISSING", 0) == 0 and len(results) == manifest["expected_total"],
        "expected_total": manifest["expected_total"],
    }
    (results_dir / "run_validation.json").write_text(json.dumps(payload, indent=2) + "\n")
    print(f"L3_COUNTS={json.dumps(counts, sort_keys=True)}")
    print(f"L3_RUN={'PASS' if payload['pass'] else 'FAIL'}")
    return 0 if payload["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
