#!/usr/bin/env python3
"""Run and validate the G3 SECDED/storage experiments."""
from __future__ import annotations

import csv
import hashlib
import json
import os
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIM_DIR = ROOT / "simulator"
SIM = SIM_DIR / "sim"
RUNS = ROOT / "runs/g3"
RESULTS = ROOT / "results/g3"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    RUNS.mkdir(parents=True, exist_ok=True)
    RESULTS.mkdir(parents=True, exist_ok=True)
    binary_sha = sha256(SIM)
    patch_sha = sha256(ROOT / "environment/source.patch")
    manifest = {
        "started_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "binary_sha256": binary_sha,
        "patch_sha256": patch_sha,
    }
    failures = 0

    ecc_csv = RUNS / "ecc_cpp_exhaustive.csv"
    witness = RUNS / "f1_witness.json"
    env = os.environ.copy()
    env["PHASE1_ECC_OUT"] = str(ecc_csv)
    env["PHASE1_G3_WITNESS"] = str(witness)
    start = time.time()
    ecc = subprocess.run([str(SIM), "--gtest_filter=Phase1Ecc.*:Phase1Storage.*"],
                         cwd=SIM_DIR, env=env, text=True, capture_output=True, timeout=120)
    (RUNS / "ecc_tests.stdout.log").write_text(ecc.stdout)
    (RUNS / "ecc_tests.stderr.log").write_text(ecc.stderr)
    (RUNS / "ecc_tests.exit_code").write_text(f"{ecc.returncode}\n")
    manifest["ecc_test_exit_code"] = ecc.returncode
    manifest["ecc_wall_seconds"] = round(time.time() - start, 3)
    if ecc.returncode != 0:
        failures += 1

    pytest = subprocess.run([sys.executable, "-m", "pytest", "-q",
                             "tests/test_ecc_reference.py"],
                            cwd=ROOT, text=True, capture_output=True, timeout=120)
    (RUNS / "ecc_python.stdout.log").write_text(pytest.stdout)
    (RUNS / "ecc_python.stderr.log").write_text(pytest.stderr)
    (RUNS / "ecc_python.exit_code").write_text(f"{pytest.returncode}\n")
    manifest["python_test_exit_code"] = pytest.returncode
    if pytest.returncode != 0:
        failures += 1

    rows = []
    if ecc_csv.exists():
        with ecc_csv.open(newline="") as fh:
            rows = list(csv.DictReader(fh))
    kinds = {}
    statuses = {}
    for row in rows:
        kinds[row["flip_kind"]] = kinds.get(row["flip_kind"], 0) + 1
        statuses[row["status"]] = statuses.get(row["status"], 0) + 1
    expected_rows = 8 * (1 + 72 + 2556)
    single_ok = all(r["status"] == "CORRECTED" and r["corrected_matches_original"] == "1"
                    for r in rows if r["flip_kind"] == "single")
    double_ok = all(r["status"] == "UNCORRECTABLE" for r in rows if r["flip_kind"] == "double")
    clean_ok = all(r["status"] == "CLEAN" and r["corrected_matches_original"] == "1"
                   for r in rows if r["flip_kind"] == "none")
    checks = {
        "cpp_exit_zero": ecc.returncode == 0,
        "python_exit_zero": pytest.returncode == 0,
        "csv_row_count": len(rows) == expected_rows,
        "csv_single_count": kinds.get("single", 0) == 8 * 72,
        "csv_double_count": kinds.get("double", 0) == 8 * 2556,
        "csv_clean_count": kinds.get("none", 0) == 8,
        "single_all_corrected": single_ok,
        "double_all_uncorrectable": double_ok,
        "clean_all_clean": clean_ok,
        "witness_exists": witness.exists(),
    }
    witness_data = json.loads(witness.read_text()) if witness.exists() else {}
    checks["witness_corrected"] = witness_data.get("status") == "CORRECTED"
    checks["witness_data_equal"] = witness_data.get("corrected_equals_original") is True
    checks["witness_no_scrub"] = witness_data.get("scrubbed") is False
    checks["witness_second_read_ce"] = witness_data.get("second_read_status") == "CORRECTED"
    checks["witness_legal_write_clean"] = witness_data.get("legal_write_status") == "CLEAN"

    summary = {
        "binary_sha256": binary_sha,
        "patch_sha256": patch_sha,
        "ecc_rows": len(rows),
        "flip_kinds": kinds,
        "statuses": statuses,
        "witness": witness_data,
        "checks": checks,
        "pass": all(checks.values()) and failures == 0,
    }
    (RESULTS / "ecc_summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    (RESULTS / "validation.json").write_text(json.dumps(summary, indent=2) + "\n")
    manifest["finished_utc"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    manifest["pass"] = summary["pass"]
    (RUNS / "run_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for key, value in checks.items():
        print(("PASS" if value else "FAIL"), key)
    print("G3_RUNNER=" + ("PASS" if summary["pass"] else "FAIL"))
    return 0 if summary["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
