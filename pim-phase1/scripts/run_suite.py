#!/usr/bin/env python3
"""Execute the hash-addressed paper manifest and reconcile every case."""
from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
SIM_DIR = ROOT / "simulator"
SIM = SIM_DIR / "sim"
RUNS = ROOT / "runs/paper"
RESULTS = ROOT / "results/g8"
UPSTREAM_COMMIT = "3703d1f19c8f027360cc33a3243eb271e3bb6898"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


G4 = load_module("g4_validator", ROOT / "scripts/run_g4.py")
G6 = load_module("g6_validator", ROOT / "scripts/run_g6.py")
G7 = load_module("g7_validator", ROOT / "scripts/run_g7.py")


def expected_words(n_tiles: int, seed: int) -> np.ndarray:
    dim = n_tiles * 128
    index = np.arange(dim * 16, dtype=np.int64)
    a = (((index + seed) % 17 - 8).astype(np.float32) / np.float32(8.0)).astype(np.float16)
    x = (((3 * index + seed) % 13 - 6).astype(np.float32) / np.float32(8.0)).astype(np.float16)
    return (a + x).astype(np.float16)


def output_matches(path: Path, expected: np.ndarray) -> bool:
    if not path.exists():
        return False
    observed = np.frombuffer(path.read_bytes(), dtype="<u2")
    return observed.shape == expected.shape and np.array_equal(
        observed.view(np.uint16), expected.view(np.uint16))


def validate_p1c(case: dict, run_dir: Path, metrics: dict) -> tuple[bool, dict]:
    dim = int(case["n_tiles"]) * 128
    consumed = metrics.get("consumed") is True
    published = metrics.get("published") is True
    checks = {
        "no_timeout": metrics.get("timeout") is False,
        "contract_pass": metrics.get("contract_pass") is True,
        "report_accounting": (
            metrics.get("delivered_reports", 0) <= metrics.get("detected_events", 0)
            and metrics.get("dropped_reports", 0) == 0),
        "published_consumed_consistent": (
            (published and consumed) or (not published and not consumed)),
        "consumer_bit_exact": (
            metrics.get("outcome_class") == "FORMAT_UNSUPPORTED"
            or (not consumed) or metrics.get("output_bit_exact") is True),
        "output_size": (not consumed)
        or (run_dir / "output.bin").stat().st_size == dim * 32,
        "quiesce_when_locked": (
            metrics.get("lock_cycle", 0) == 0
            or (metrics.get("quiesce_ack_cycle", 0) >= metrics.get("pause_request_cycle", 0)
                and metrics.get("quiesce_ack_cycle", 0) > 0)),
        "consumer_path_matches": (
            (metrics.get("consumer") == "HOST"
             and (not consumed or metrics.get("host_read") is True))
            or (metrics.get("consumer") == "PIM"
                and (not consumed or metrics.get("pim_read") is True))),
        "outcome_class_known": metrics.get("outcome_class") in {
            "TIMELY_CONTROL", "LATE_CONTROL", "NO_REPORT", "POISONED", "TIMEOUT",
            "INVALID_CASE", "FORMAT_UNSUPPORTED"},
        "parity_bytes_present": metrics.get("parity_bytes", 0) > 0,
        "version_metadata_present": metrics.get("runtime_metadata_bytes", 0) > 0,
    }
    if consumed and metrics.get("outcome_class") != "FORMAT_UNSUPPORTED":
        if metrics.get("consumer") == "HOST":
            checks["independent_output"] = output_matches(
                run_dir / "output.bin", expected_words(int(case["n_tiles"]), int(case["seed"])))
        else:
            expected_b = expected_words(int(case["n_tiles"]), int(case["seed"]))
            expected_c = (expected_b * np.float16(2.0)).astype(np.float16)
            checks["independent_output"] = output_matches(run_dir / "output.bin", expected_c)
    return all(checks.values()), checks


def validate_cost(case: dict, run_dir: Path, metrics: dict) -> tuple[bool, dict]:
    dim = int(case["n_tiles"]) * 128
    expected_b = expected_words(int(case["n_tiles"]), int(case["seed"]))
    expected_c = (expected_b * np.float16(2.0)).astype(np.float16)
    expected = expected_b if case["consumer"] == "HOST" else expected_c
    protected = case["policy"] != "EXT_OFF"
    checks = {
        "no_timeout": metrics.get("timeout") is False,
        "published": metrics.get("published") is True,
        "consumed": metrics.get("consumed") is True,
        "output_bit_exact": metrics.get("output_bit_exact") is True,
        "independent_output": output_matches(run_dir / "output.bin", expected),
        "output_size": (run_dir / "output.bin").stat().st_size == dim * 32,
        "producer_cycles": metrics.get("producer_cycles", 0) > 0,
        "end_to_end_ge_producer": (
            metrics.get("end_to_end_cycles", 0) >= metrics.get("producer_cycles", 0)),
        "no_fault_reports": metrics.get("detected_events") == 0,
        "ext_off_metadata_zero": (
            protected
            or (metrics.get("parity_bytes") == 0
                and metrics.get("runtime_metadata_bytes") == 0
                and metrics.get("peak_held_output_bytes") == 0)),
        "protected_metadata_present": (
            not protected
            or (metrics.get("parity_bytes", 0) > 0
                and metrics.get("runtime_metadata_bytes", 0) > 0
                and metrics.get("version", 0) >= 1)),
    }
    return all(checks.values()), checks


def validate_robust(case: dict, run_dir: Path, metrics: dict) -> tuple[bool, dict]:
    drop_rate = case["drop_rate"]
    checks = {
        "no_timeout": metrics.get("timeout") is False,
        "contract_pass": metrics.get("contract_pass") is True,
        "accounting": metrics.get("accounting_ok") is True,
        "all_detections": metrics.get("total_detections") == int(case["repeat_reads"]),
        "delivery_accounting": (
            metrics.get("delivered_reports", 0) + metrics.get("queue_drops", 0)
            == metrics.get("forwarded_detections", 0)),
        "injected_drop_count": (
            metrics.get("injected_drops") == 0 if drop_rate == "0"
            else metrics.get("injected_drops") == 3),
        "injected_drop_sequences": (
            metrics.get("dropped_detection_seqs") == []
            if drop_rate == "0"
            else metrics.get("dropped_detection_seqs") == [10, 20, 30]),
        "service_interval": metrics.get("service_interval_ok") is True,
        "reports_delivered": metrics.get("delivered_reports", 0) > 0,
        "latency_floor": (
            not metrics.get("report_latency_cycles")
            or min(metrics["report_latency_cycles"]) >= max(
                1, int(case["transport_delay_cycles"]))),
        "report_cycles_monotonic": all(
            b >= a for a, b in zip(metrics.get("report_cycles", []),
                                   metrics.get("report_cycles", [])[1:])),
    }
    return all(checks.values()), checks


def read_csv_rows(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def validate_p1g(case: dict, run_dir: Path, metrics: dict) -> tuple[bool, dict]:
    params = case["params"]
    n_tiles = int(params["n_tiles"])
    dim = n_tiles * 128
    fault = params["fault_kind"]
    placement = params["escape_placement"]
    reports = read_csv_rows(run_dir / "reports.csv")
    truth = read_csv_rows(run_dir / "truth.csv")
    injections = [row for row in truth if row["record_type"] == "INJECTION"]
    status_by_access = {
        row["target_access_id"]: row["decoder_status"] for row in injections}
    corrupt_burst = metrics.get("corrupt_burst")
    witness_burst = metrics.get("witness_burst")
    region_ids = [int(row["region_id"]) for row in reports]
    published = metrics.get("published") is True
    consumed = metrics.get("consumed") is True
    if metrics.get("timeout") is True:
        expected_escape = "TIMEOUT"
    elif metrics.get("corruption_propagates") is not True:
        expected_escape = "MASKED"
    elif consumed and metrics.get("output_changed") is True:
        expected_escape = "CORRUPT_CONSUMED"
    elif (not published and int(metrics.get("lock_cycle") or 0) != 0
          and metrics.get("held_output_corrupt") is True):
        expected_escape = "PREVENTED"
    else:
        expected_escape = "UNCLASSIFIED"

    expected_injections = 2 if fault == "F1W_F4S" else 1
    if corrupt_burst is None:
        corrupt_status = None
    else:
        corrupt_status = status_by_access.get(f"A_burst_{corrupt_burst}")
    if witness_burst is None:
        witness_status = None
    else:
        witness_status = status_by_access.get(f"A_burst_{witness_burst}")
    corrupt_bank = metrics.get("corrupt_bank")
    expected_cover = (corrupt_bank is not None
                      and any(row["region_type"] == "bank"
                              and int(row["region_id"]) == int(corrupt_bank)
                              for row in reports))
    output_changed_ok = (
        (metrics.get("output_changed") is (not metrics.get("output_bit_exact")))
        if consumed else metrics.get("output_changed") is None)
    output_path = run_dir / "output.bin"
    checks = {
        "no_timeout": metrics.get("timeout") is False,
        "contract_pass": metrics.get("contract_pass") is True,
        "escape_outcome_known": metrics.get("escape_outcome") in {
            "PREVENTED", "CORRUPT_CONSUMED", "MASKED"},
        "escape_outcome_consistency": metrics.get("escape_outcome") == expected_escape,
        "injection_count": len(injections) == expected_injections,
        "report_count": len(reports) == metrics.get("delivered_reports"),
        "drop_accounting": metrics.get("dropped_reports") == 0,
        "delivery_accounting": (
            metrics.get("delivered_reports", 0) <= metrics.get("detected_events", 0)),
        "internal_ue_zero": metrics.get("internal_ue") == 0,
        "report_region_ids": region_ids == metrics.get("report_region_ids"),
        "report_covers_corrupt_bank": (
            metrics.get("report_covers_corrupt_bank") == expected_cover),
        "published_consumed_consistent": (
            (published and consumed) or (not published and not consumed)),
        "output_changed_consistent": output_changed_ok,
        "held_output_published_null": (not published or metrics.get("held_output_corrupt") is None),
        "output_size": (not consumed) or (
            output_path.exists() and output_path.stat().st_size == dim * 32),
    }
    if fault == "F3M":
        checks["f3m_detected"] = metrics.get("detected_events", 0) >= 1
        checks["f3m_decoder_corrected"] = corrupt_status == "CORRECTED"
    elif fault == "F4S":
        checks["f4s_no_detection"] = (
            metrics.get("detected_events") == 0
            and metrics.get("delivered_reports") == 0)
        checks["f4s_decoder_clean"] = corrupt_status == "CLEAN"
    else:
        checks["f1w_distinct_bursts"] = (
            corrupt_burst is not None and witness_burst is not None
            and corrupt_burst != witness_burst)
        checks["f1w_detected"] = metrics.get("detected_events", 0) >= 1
        checks["f1w_decoder_statuses"] = (
            witness_status == "CORRECTED" and corrupt_status == "CLEAN")
    return all(checks.values()), checks


def validate_p1h(case: dict, run_dir: Path, metrics: dict) -> tuple[bool, dict]:
    params = case["params"]
    scenario = params["scenario"]
    granularity = params["granularity"]
    scrub = int(params["scrub"])
    reports = read_csv_rows(run_dir / "reports.csv")
    truth = read_csv_rows(run_dir / "truth.csv")
    injections = [row for row in truth if row["record_type"] == "INJECTION"]
    detections = [row for row in truth if row["record_type"] == "DETECTION"]
    expected_injections = 1 if scenario == "S1_SINGLE" else 2
    expected_scrub = 0 if scrub == 0 else (1 if scenario == "S1_SINGLE" else 2)
    signature = ";".join(
        f"{row['error_class']}:{row['region_id']}" for row in reports)
    region_ok = True
    if granularity in ("bank", "codeword"):
        expected_ids = []
        for detection in detections:
            fields = dict(part.split("=") for part in detection["storage_key"].split(","))
            bank = int(fields["bank"])
            row = int(fields["row"])
            column = int(fields["burst_col"])
            codeword = int(fields["cw"])
            if granularity == "bank":
                expected_ids.append(bank)
            else:
                expected_ids.append(
                    (((bank * 16384 + row) * 32 + column) * 4 + codeword))
        region_ok = sorted(expected_ids) == sorted(int(row["region_id"]) for row in reports)
    checks = {
        "no_timeout": metrics.get("timeout") is False,
        "contract_pass": metrics.get("contract_pass") is True,
        "injection_count": len(injections) == expected_injections,
        "detection_count": len(detections) == metrics.get("detected_events"),
        "report_count": len(reports) == metrics.get("delivered_reports"),
        "delivery_accounting": (
            metrics.get("delivered_reports") == metrics.get("detected_events")),
        "internal_accounting": (
            metrics.get("internal_ce", 0) + metrics.get("internal_ue", 0)
            == metrics.get("detected_events")),
        "report_signature": metrics.get("report_signature") == signature,
        "region_type": all(row["region_type"] == granularity for row in reports),
        "region_ids": region_ok,
        "scrub_writes": metrics.get("scrub_writes") == expected_scrub,
        "rounds": metrics.get("rounds") == 4,
        "s6_scrub": scenario != "S6_SAME_BIT_RECUR" or scrub == 1,
    }
    return all(checks.values()), checks


def validate_case(case: dict) -> tuple[bool, dict, dict]:
    run_dir = Path(case["case"]["out"])
    metrics = json.loads((run_dir / "metrics.json").read_text())
    group = case["group"]
    case_meta = dict(case["case"])
    case_meta["out"] = run_dir
    if group in ("P1-A", "P1-B"):
        passed, checks, _ = G4.validate_case(case_meta)
    elif group == "P1-C":
        passed, checks = validate_p1c(case_meta, run_dir, metrics)
    elif group == "P1-D":
        passed, checks, _ = G6.validate_case(case_meta)
    elif group == "P1-E":
        kind = case_meta.get("kind")
        if kind == "positive":
            passed, checks = G7.validate_positive(case_meta, metrics, run_dir)
        elif kind == "status_negative":
            passed, checks = G7.validate_status_negative(case_meta, metrics)
        else:
            passed, checks = G7.validate_negative_control(case_meta, metrics)
    elif group == "P1-F Cost":
        passed, checks = validate_cost(case_meta, run_dir, metrics)
    elif group == "P1-F Robustness":
        passed, checks = validate_robust(case_meta, run_dir, metrics)
    elif group == "P1-G Escape":
        passed, checks = validate_p1g(case, run_dir, metrics)
    elif group == "P1-H Sequence":
        passed, checks = validate_p1h(case, run_dir, metrics)
    else:
        raise ValueError(f"unknown group: {group}")
    return bool(passed), checks, metrics


def read_existing(case: dict, binary_sha: str, patch_sha: str) -> tuple[bool, dict]:
    run_dir = Path(case["case"]["out"])
    metadata_path = run_dir / "metadata.json"
    validation_path = run_dir / "validation.json"
    if not metadata_path.exists() or not validation_path.exists():
        return False, {}
    try:
        metadata = json.loads(metadata_path.read_text())
        validation = json.loads(validation_path.read_text())
    except Exception:  # noqa: BLE001
        return False, {}
    if (metadata.get("binary_sha256") != binary_sha
            or metadata.get("patch_sha256") != patch_sha
            or metadata.get("run_status") not in ("SUCCESS", "EXPECTED_NEGATIVE")
            or validation.get("pass") is not True):
        return False, {}
    metrics_path = run_dir / "metrics.json"
    metrics = json.loads(metrics_path.read_text()) if metrics_path.exists() else {}
    return True, {
        "status": metadata["run_status"],
        "pass": True,
        "validation": validation,
        "metrics": metrics,
    }


def run_one(case: dict, binary_sha: str, patch_sha: str,
            environment_hash: str, resume: bool) -> dict:
    run_id = case["run_id"]
    run_dir = Path(case["case"]["out"])
    run_dir.mkdir(parents=True, exist_ok=True)
    params_path = run_dir / "params.ini"
    params_text = "".join(f"{k}={v}\n" for k, v in case["params"].items())
    params_path.write_text(params_text)
    (run_dir / "config.json").write_text(json.dumps(case, indent=2) + "\n")
    config_sha = sha256(params_path)
    expected_negative = bool(case["expected_negative"])

    if resume:
        ok, existing = read_existing(case, binary_sha, patch_sha)
        if ok:
            return {
                "case": case,
                "status": existing["status"],
                "pass": True,
                "metrics": existing.get("metrics", {}),
                "validation": existing["validation"],
                "wall_seconds": 0.0,
                "exit_code": 0,
                "skipped": True,
            }

    metadata = {
        "schema_version": 1,
        "run_id": run_id,
        "case_id": case["case"].get("case_id", run_id),
        "gate": "G8",
        "group": case["group"],
        "fixture": case["fixture"],
        "expected_negative": expected_negative,
        "upstream_commit": UPSTREAM_COMMIT,
        "binary_sha256": binary_sha,
        "patch_sha256": patch_sha,
        "config_sha256": config_sha,
        "environment_hash": environment_hash,
        "start_time_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "run_status": "RUNNING",
    }
    for key in ("n_tiles", "seed", "consumer", "policy", "scenario", "negative_control",
                "status", "delay_ratio", "hold_ratio"):
        if key in case["case"]:
            metadata[key] = case["case"][key]

    env = os.environ.copy()
    env["PHASE1_CONFIG"] = str(params_path)
    env["PHASE1_OUT"] = str(run_dir)
    started = time.time()
    try:
        proc = subprocess.run(
            [str(SIM), f"--gtest_filter={case['fixture']}"],
            cwd=SIM_DIR, env=env, text=True, capture_output=True, timeout=300)
        exit_code = proc.returncode
        stdout = proc.stdout
        stderr = proc.stderr
    except subprocess.TimeoutExpired as exc:
        exit_code = 124
        stdout = exc.stdout if isinstance(exc.stdout, str) else ""
        stderr = exc.stderr if isinstance(exc.stderr, str) else ""
    elapsed = time.time() - started
    (run_dir / "stdout.log").write_text(stdout)
    (run_dir / "stderr.log").write_text(stderr)
    (run_dir / "exit_code").write_text(f"{exit_code}\n")
    metadata["wall_seconds"] = round(elapsed, 3)
    metadata["exit_code"] = exit_code

    passed = False
    checks: dict = {"exit_code_zero": exit_code == 0}
    metrics: dict = {}
    validation: dict = {}
    error = ""
    if exit_code == 0:
        try:
            passed, checks, metrics = validate_case(case)
            validation = {
                "run_id": run_id,
                "group": case["group"],
                "expected_negative": expected_negative,
                "pass": passed,
                "checks": checks,
                "outcome_class": metrics.get("outcome_class"),
                "contract_pass": metrics.get("contract_pass"),
                "error": "",
            }
        except Exception as exc:  # noqa: BLE001
            error = str(exc)
    else:
        error = f"simulator exit {exit_code}"

    if exit_code == 124:
        status = "TIMEOUT"
    elif passed and expected_negative:
        status = "EXPECTED_NEGATIVE"
    elif passed:
        status = "SUCCESS"
    else:
        status = "FAIL"
    metadata["run_status"] = status
    metadata["error"] = error
    (run_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    if not validation:
        validation = {
            "run_id": run_id,
            "group": case["group"],
            "expected_negative": expected_negative,
            "pass": False,
            "checks": checks,
            "outcome_class": None,
            "contract_pass": None,
            "error": error,
        }
    (run_dir / "validation.json").write_text(json.dumps(validation, indent=2) + "\n")
    return {
        "case": case,
        "status": status,
        "pass": passed,
        "metrics": metrics,
        "validation": validation,
        "wall_seconds": elapsed,
        "exit_code": exit_code,
        "skipped": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", default=str(ROOT / "configs/generated/paper-manifest.json"))
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--group", default="")
    parser.add_argument("--results-dir", default=str(RESULTS),
                        help="directory for runner summaries (default: results/g8)")
    parser.add_argument("--runs-dir", default=str(RUNS),
                        help="directory for run-level manifest (default: runs/paper)")
    args = parser.parse_args()
    results_dir = Path(args.results_dir).resolve()
    runs_dir = Path(args.runs_dir).resolve()

    manifest_path = Path(args.manifest).resolve()
    manifest = json.loads(manifest_path.read_text())
    cases = manifest["cases"]
    if args.group:
        cases = [c for c in cases if c["group"] == args.group]
    if args.limit:
        cases = cases[:args.limit]
    runs_dir.mkdir(parents=True, exist_ok=True)
    results_dir.mkdir(parents=True, exist_ok=True)
    binary_sha = sha256(SIM)
    patch_path = ROOT / "environment/source.patch"
    patch_sha = sha256(patch_path) if patch_path.exists() else "MISSING"
    environment_hash = json.loads(
        (ROOT / "environment/environment.json").read_text())["environment_hash"]

    started = datetime.now(timezone.utc).isoformat(timespec="seconds")
    results = []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as executor:
        futures = {
            executor.submit(run_one, case, binary_sha, patch_sha, environment_hash,
                            args.resume): case
            for case in cases
        }
        for index, future in enumerate(as_completed(futures), start=1):
            result = future.result()
            results.append(result)
            print(f"[{index}/{len(cases)}] {result['case']['run_id']} "
                  f"{result['case']['group']} {result['status']} "
                  f"pass={result['pass']}")

    results.sort(key=lambda item: (item["case"]["group"], item["case"]["run_id"]))
    counts = {
        "SUCCESS": 0,
        "EXPECTED_NEGATIVE": 0,
        "FAIL": 0,
        "TIMEOUT": 0,
    }
    for result in results:
        counts[result["status"]] = counts.get(result["status"], 0) + 1

    rows = []
    failures = []
    for result in results:
        case = result["case"]
        metrics = result["metrics"]
        row = {
            "run_id": case["run_id"],
            "group": case["group"],
            "case_id": case["case"].get("case_id", ""),
            "status": result["status"],
            "pass": result["pass"],
            "expected_negative": case["expected_negative"],
            "wall_seconds": round(result["wall_seconds"], 3),
            "exit_code": result["exit_code"],
            "outcome_class": metrics.get("outcome_class", ""),
            "contract_pass": metrics.get("contract_pass", result["pass"]),
            "n_tiles": case["case"].get("n_tiles", ""),
            "seed": case["case"].get("seed", ""),
            "consumer": case["case"].get("consumer", ""),
            "policy": case["case"].get("policy", ""),
            "producer_cycles": metrics.get("producer_cycles", ""),
            "end_to_end_cycles": metrics.get("end_to_end_cycles", ""),
            "detected_events": metrics.get("detected_events", ""),
            "delivered_reports": metrics.get("delivered_reports", ""),
            "queue_drops": metrics.get("queue_drops", ""),
            "injected_drops": metrics.get("injected_drops", ""),
            "output_bit_exact": metrics.get("output_bit_exact", ""),
            "published": metrics.get("published", ""),
            "consumed": metrics.get("consumed", ""),
            "quiesce_cycles": metrics.get("quiesce_cycles", ""),
            "error": result["validation"].get("error", ""),
        }
        rows.append(row)
        if result["status"] in ("FAIL", "TIMEOUT"):
            failures.append(row)

    fieldnames = list(rows[0].keys()) if rows else []
    with (results_dir / "run_metrics.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    with (results_dir / "failure_cases.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(failures)

    summary_rows = []
    for group in sorted({row["group"] for row in rows}):
        group_rows = [row for row in rows if row["group"] == group]
        summary_rows.append({
            "group": group,
            "cases": len(group_rows),
            "success": sum(1 for row in group_rows if row["status"] == "SUCCESS"),
            "expected_negative": sum(
                1 for row in group_rows if row["status"] == "EXPECTED_NEGATIVE"),
            "fail": sum(1 for row in group_rows if row["status"] == "FAIL"),
            "timeout": sum(1 for row in group_rows if row["status"] == "TIMEOUT"),
            "contract_pass": sum(1 for row in group_rows if row["contract_pass"] is True),
        })
    with (results_dir / "summary.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summary_rows[0].keys())
                                if summary_rows else [])
        writer.writeheader()
        writer.writerows(summary_rows)

    total = len(results)
    manifest_total = manifest["expected_total"]
    selected_expected_total = len(cases)
    selected_negative_total = sum(1 for case in cases if case["expected_negative"])
    full_manifest = not args.limit and not args.group
    checks = {
        "manifest_case_count": total == selected_expected_total,
        "full_manifest_case_count": (not full_manifest) or total == manifest_total,
        "count_reconciliation": (
            total == counts["SUCCESS"] + counts["EXPECTED_NEGATIVE"] + counts["FAIL"]
            + counts["TIMEOUT"]),
        "expected_negative_count": counts["EXPECTED_NEGATIVE"] == selected_negative_total,
        "no_failures": counts["FAIL"] == 0,
        "no_timeouts": counts["TIMEOUT"] == 0,
        "all_success_cases_pass": all(
            result["pass"] for result in results if result["status"] == "SUCCESS"),
        "all_expected_negatives_pass": all(
            result["pass"] for result in results if result["status"] == "EXPECTED_NEGATIVE"),
    }
    validation = {
        "gate": "G8",
        "pass": all(checks.values()),
        "checks": checks,
        "manifest": str(manifest_path.relative_to(ROOT)),
        "manifest_sha256": sha256(manifest_path),
        "binary_sha256": binary_sha,
        "patch_sha256": patch_sha,
        "started_utc": started,
        "finished_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "manifest_total": manifest_total,
        "selected_total": selected_expected_total,
        "executed_total": total,
        "expected_negative": selected_negative_total,
        "counts": counts,
        "failure_count": len(failures),
    }
    (results_dir / "validation.json").write_text(json.dumps(validation, indent=2) + "\n")
    (runs_dir / "run_manifest.json").write_text(json.dumps({
        "gate": "G8",
        "manifest": str(manifest_path.relative_to(ROOT)),
        "manifest_sha256": sha256(manifest_path),
        "binary_sha256": binary_sha,
        "patch_sha256": patch_sha,
        "started_utc": started,
        "finished_utc": validation["finished_utc"],
        "counts": counts,
        "cases": [
            {
                "run_id": result["case"]["run_id"],
                "group": result["case"]["group"],
                "status": result["status"],
                "pass": result["pass"],
                "exit_code": result["exit_code"],
                "expected_negative": result["case"]["expected_negative"],
            }
            for result in results
        ],
    }, indent=2) + "\n")
    for key, value in checks.items():
        print(("PASS" if value else "FAIL"), key)
    print("G8_RUNNER=" + ("PASS" if validation["pass"] else "FAIL"))
    return 0 if validation["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
