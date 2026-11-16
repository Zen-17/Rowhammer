#!/usr/bin/env python3
"""Run the G1 no-fault cases in separate processes and record commands/results."""
from __future__ import annotations

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
RUNS = ROOT / "runs/g1"
DEVICE_INI = SIM_DIR / "ini/HBM2_samsung_2M_16B_x64.ini"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def params_text(case: dict) -> str:
    system_ini = ROOT / f"configs/phase1_system_{case['channels']}ch.ini"
    capacity = 256 if case["channels"] == 1 else 512
    return f"""schema_version=1
experiment=G1
workload={case['workload']}
mode={case['mode']}
channels={case['channels']}
ranks=1
capacity_mb={capacity}
n_tiles={case['n_tiles']}
seed={case['seed']}
feeder_outstanding_limit=8
feeder_issue_per_cycle=1
watchdog_cycles=100000
device_ini={DEVICE_INI}
system_ini={system_ini}
output_dir={case['out']}
"""


def cases() -> list[dict]:
    cases = []
    for channel in (1, 2):
        cases.append({
            "case_id": f"address_{channel}ch",
            "workload": "ADDRESS",
            "mode": "direct",
            "channels": channel,
            "n_tiles": 0,
            "seed": 0,
            "out": RUNS / "address_mapping" / f"{channel}ch",
        })
    for mode in ("direct", "feeder"):
        cases.append({
            "case_id": f"w0_1ch_{mode}",
            "workload": "W0",
            "mode": mode,
            "channels": 1,
            "n_tiles": 0,
            "seed": 0,
            "out": RUNS / f"w0_1ch_{mode}",
        })
    for n_tiles in (1, 8):
        for seed in (0, 1, 2):
            for mode in ("direct", "feeder"):
                cases.append({
                    "case_id": f"W1_1ch_nt{n_tiles}_seed{seed}_{mode}",
                    "workload": "W1",
                    "mode": mode,
                    "channels": 1,
                    "n_tiles": n_tiles,
                    "seed": seed,
                    "out": RUNS / f"w1_1ch_nt{n_tiles}_seed{seed}_{mode}",
                })
    return cases


def main() -> int:
    if not SIM.exists():
        print(f"missing simulator binary: {SIM}", file=sys.stderr)
        return 2
    recorded_binary = (ROOT / "environment/g1_binary.sha256").read_text().split()[0]
    actual_binary = sha256(SIM)
    if recorded_binary != actual_binary:
        print(f"binary hash mismatch: recorded {recorded_binary}, actual {actual_binary}", file=sys.stderr)
        return 3

    RUNS.mkdir(parents=True, exist_ok=True)
    started = datetime.now(timezone.utc).isoformat(timespec="seconds")
    manifest = {"started_utc": started, "binary_sha256": actual_binary, "cases": []}
    failures = 0
    for case in cases():
        run = case["out"]
        run.mkdir(parents=True, exist_ok=True)
        params = run / "params.ini"
        params.write_text(params_text(case))
        env = os.environ.copy()
        env["PHASE1_CONFIG"] = str(params)
        env["PHASE1_OUT"] = str(run)
        command = [str(SIM), "--gtest_filter=Phase1Fixture.RunCase"]
        start = time.time()
        try:
            proc = subprocess.run(command, cwd=SIM_DIR, env=env, text=True, capture_output=True,
                                  timeout=120)
            exit_code = proc.returncode
            stdout = proc.stdout
            stderr = proc.stderr
            timed_out = False
        except subprocess.TimeoutExpired as exc:
            exit_code = 124
            stdout = (exc.stdout or "") if isinstance(exc.stdout, str) else ""
            stderr = (exc.stderr or "") if isinstance(exc.stderr, str) else ""
            timed_out = True
        elapsed = time.time() - start
        (run / "stdout.log").write_text(stdout)
        (run / "stderr.log").write_text(stderr)
        (run / "exit_code").write_text(f"{exit_code}\n")
        metadata = {
            "case_id": case["case_id"],
            "workload": case["workload"],
            "mode": case["mode"],
            "channels": case["channels"],
            "n_tiles": case["n_tiles"],
            "seed": case["seed"],
            "command": " ".join(command),
            "working_directory": str(SIM_DIR),
            "environment": {"PHASE1_CONFIG": str(params), "PHASE1_OUT": str(run)},
            "exit_code": exit_code,
            "timed_out": timed_out,
            "wall_seconds": round(elapsed, 3),
            "binary_sha256": actual_binary,
            "params_sha256": sha256(params),
            "started_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        }
        (run / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        manifest["cases"].append(metadata)
        print(f"{case['case_id']}: exit={exit_code} wall={elapsed:.3f}s")
        if exit_code != 0:
            failures += 1

    validation = subprocess.run([sys.executable, str(ROOT / "scripts/g1_validate.py")],
                                cwd=ROOT, text=True, capture_output=True)
    (RUNS / "validation_stdout.log").write_text(validation.stdout)
    (RUNS / "validation_stderr.log").write_text(validation.stderr)
    manifest["validation_exit_code"] = validation.returncode
    manifest["validation_stdout_sha256"] = hashlib.sha256(validation.stdout.encode()).hexdigest()
    manifest["case_failures"] = failures
    manifest["finished_utc"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    (RUNS / "run_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(validation.stdout)
    if validation.stderr:
        print(validation.stderr, file=sys.stderr)
    return 0 if failures == 0 and validation.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
