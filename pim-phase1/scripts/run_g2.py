#!/usr/bin/env python3
"""Run G2 no-fault trace comparisons: feeder EXT_OFF vs EXT_ON."""
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
RUNS = ROOT / "runs/g2"
RESULTS = ROOT / "results/g2"
DEVICE_INI = SIM_DIR / "ini/HBM2_samsung_2M_16B_x64.ini"
UPSTREAM_COMMIT = "3703d1f19c8f027360cc33a3243eb271e3bb6898"
REPORT_HEADER = ["report_seq", "report_cycle", "error_class", "region_type", "region_id"]
TRUTH_HEADER = [
    "record_type", "detection_id", "fault_id", "fault_cycle", "storage_key", "codeword_index",
    "bit_positions", "raw_before", "raw_after", "target_access_id", "detect_cycle",
    "decoder_status", "eligible_report_id", "dropped",
]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_header(path: Path, header: list[str]) -> None:
    with path.open("w", newline="") as fh:
        csv.writer(fh).writerow(header)


def reference_inputs(n_tiles: int, seed: int):
    dim = n_tiles * 16 * 1 * 1 * 8
    index = np.arange(dim * 16, dtype=np.int64)
    a = (((index + seed) % 17 - 8).astype(np.float32) / np.float32(8.0)).astype(np.float16)
    x = (((3 * index + seed) % 13 - 6).astype(np.float32) / np.float32(8.0)).astype(np.float16)
    return a, x, dim


def params_text(case: dict) -> str:
    return f"""schema_version=1
experiment=G2
workload=W1
mode=feeder
trace_mode={case['trace_mode']}
channels=1
ranks=1
capacity_mb=256
n_tiles={case['n_tiles']}
seed={case['seed']}
feeder_outstanding_limit=8
feeder_issue_per_cycle=1
watchdog_cycles=100000
dim_bursts={case['dim_bursts']}
elements={case['elements']}
device_ini={DEVICE_INI}
system_ini={ROOT}/configs/phase1_system_1ch.ini
output_dir={case['out']}
"""


def canonical_hash(case: dict, a: np.ndarray, x: np.ndarray) -> str:
    canonical = {
        "experiment": "G2",
        "workload": "W1",
        "mode": "feeder",
        "trace_mode": case["trace_mode"],
        "channels": 1,
        "ranks": 1,
        "capacity_mb": 256,
        "n_tiles": case["n_tiles"],
        "seed": case["seed"],
        "feeder_outstanding_limit": 8,
        "feeder_issue_per_cycle": 1,
        "watchdog_cycles": 100000,
        "dim_bursts": case["dim_bursts"],
        "elements": case["elements"],
        "device_ini": str(DEVICE_INI),
        "system_ini": str(ROOT / "configs/phase1_system_1ch.ini"),
    }
    payload = json.dumps(canonical, sort_keys=True, separators=(",", ":")).encode()
    payload += sha256(ROOT / "environment/source.patch").encode()
    payload += sha256(SIM).encode()
    payload += hashlib.sha256(a.view("<u2").tobytes()).digest()
    payload += hashlib.sha256(x.view("<u2").tobytes()).digest()
    return hashlib.sha256(payload).hexdigest()


def cases() -> list[dict]:
    result = []
    for n_tiles in (1, 8):
        for trace_mode in ("ext_off", "ext_on"):
            dim = n_tiles * 16 * 1 * 1 * 8
            out = RUNS / f"w1_nt{n_tiles}_seed0_feeder_{trace_mode}"
            result.append({
                "case_id": f"W1_1ch_nt{n_tiles}_seed0_feeder_{trace_mode}",
                "n_tiles": n_tiles,
                "seed": 0,
                "trace_mode": trace_mode,
                "dim_bursts": dim,
                "elements": dim * 16,
                "out": out,
            })
    return result


def main() -> int:
    if not SIM.exists():
        print(f"missing simulator: {SIM}", file=sys.stderr)
        return 2
    binary_sha = sha256(SIM)
    patch_sha = sha256(ROOT / "environment/source.patch")
    environment_hash = json.loads((ROOT / "environment/environment.json").read_text())["environment_hash"]
    RUNS.mkdir(parents=True, exist_ok=True)
    RESULTS.mkdir(parents=True, exist_ok=True)
    manifest = {"started_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                "binary_sha256": binary_sha, "patch_sha256": patch_sha, "cases": []}
    failures = 0

    for case in cases():
        out = case["out"]
        out.mkdir(parents=True, exist_ok=True)
        params = out / "params.ini"
        params.write_text(params_text(case))
        a, x, _ = reference_inputs(case["n_tiles"], case["seed"])
        run_id = canonical_hash(case, a, x)
        config = {
            "schema_version": 1, "experiment": "G2", "workload": "W1", "mode": "feeder",
            "trace_mode": case["trace_mode"], "channels": 1, "ranks": 1, "capacity_mb": 256,
            "n_tiles": case["n_tiles"], "seed": case["seed"],
            "feeder_outstanding_limit": 8, "feeder_issue_per_cycle": 1,
            "watchdog_cycles": 100000, "dim_bursts": case["dim_bursts"],
            "elements": case["elements"], "device_ini": str(DEVICE_INI),
            "system_ini": str(ROOT / "configs/phase1_system_1ch.ini"),
            "output_dir": str(out),
        }
        (out / "config.json").write_text(json.dumps(config, indent=2) + "\n")
        write_header(out / "reports.csv", REPORT_HEADER)
        write_header(out / "truth.csv", TRUTH_HEADER)
        metadata = {
            "schema_version": 1,
            "run_id": run_id[:16],
            "run_id_full": run_id,
            "experiment": "G2",
            "seed": case["seed"],
            "upstream_commit": UPSTREAM_COMMIT,
            "patch_sha256": patch_sha,
            "binary_sha256": binary_sha,
            "config_sha256": sha256(params),
            "reference_id": f"W1_1ch_nt{case['n_tiles']}_seed{case['seed']}",
            "environment_hash": environment_hash,
            "start_time_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "wall_seconds": 0.0,
            "exit_code": 0,
            "run_status": "RUNNING",
        }
        (out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        env = os.environ.copy()
        env["PHASE1_CONFIG"] = str(params)
        env["PHASE1_OUT"] = str(out)
        command = [str(SIM), "--gtest_filter=Phase1Fixture.RunCase"]
        start = time.time()
        try:
            proc = subprocess.run(command, cwd=SIM_DIR, env=env, text=True, capture_output=True,
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
        metadata["wall_seconds"] = round(elapsed, 3)
        metadata["exit_code"] = exit_code
        (out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

        validation = subprocess.run(
            [sys.executable, str(ROOT / "scripts/validate_run.py"), "--run-dir", str(out)],
            cwd=ROOT, text=True, capture_output=True)
        (out / "validation_stdout.log").write_text(validation.stdout)
        (out / "validation_stderr.log").write_text(validation.stderr)
        (out / "validation_exit_code").write_text(f"{validation.returncode}\n")
        print(f"{case['case_id']}: sim={exit_code} validation={validation.returncode}")
        if exit_code != 0 or validation.returncode != 0:
            failures += 1
        manifest["cases"].append({
            "case_id": case["case_id"], "run_id": run_id[:16],
            "sim_exit_code": exit_code, "validation_exit_code": validation.returncode,
            "wall_seconds": round(elapsed, 3), "trace_mode": case["trace_mode"],
        })

    comparisons = []
    checks = {}
    for n_tiles in (1, 8):
        off = RUNS / f"w1_nt{n_tiles}_seed0_feeder_ext_off"
        on = RUNS / f"w1_nt{n_tiles}_seed0_feeder_ext_on"
        g1 = ROOT / f"runs/g1/w1_1ch_nt{n_tiles}_seed0_feeder"
        off_timing = json.loads((off / "timing.json").read_text())
        on_timing = json.loads((on / "timing.json").read_text())
        g1_timing = json.loads((g1 / "timing.json").read_text())
        off_out = (off / "output.bin").read_bytes()
        on_out = (on / "output.bin").read_bytes()
        g1_out = (g1 / "output.bin").read_bytes()
        comparisons.append({
            "case_id": f"W1_1ch_nt{n_tiles}_seed0",
            "ext_off_cycles": off_timing["cycles"],
            "ext_on_cycles": on_timing["cycles"],
            "g1_feeder_cycles": g1_timing["cycles"],
            "cycles_ext_equal": off_timing["cycles"] == on_timing["cycles"],
            "cycles_match_g1": off_timing["cycles"] == g1_timing["cycles"],
            "output_ext_equal": off_out == on_out,
            "output_match_g1": off_out == g1_out,
        })
        checks[f"nt{n_tiles}_ext_off_on_cycles"] = off_timing["cycles"] == on_timing["cycles"]
        checks[f"nt{n_tiles}_ext_off_g1_cycles"] = off_timing["cycles"] == g1_timing["cycles"]
        checks[f"nt{n_tiles}_ext_off_on_output"] = off_out == on_out
        checks[f"nt{n_tiles}_ext_off_g1_output"] = off_out == g1_out

    with (RESULTS / "comparison.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(comparisons[0]))
        writer.writeheader()
        writer.writerows(comparisons)
    result = {"gate": "G2", "pass": all(checks.values()) and failures == 0,
              "checks": checks, "sim_failures": failures, "comparisons": comparisons,
              "binary_sha256": binary_sha, "patch_sha256": patch_sha}
    (RESULTS / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
    (RUNS / "run_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for key, value in checks.items():
        print(("PASS" if value else "FAIL"), key)
    print("G2_RUNNER=" + ("PASS" if result["pass"] else "FAIL"))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
