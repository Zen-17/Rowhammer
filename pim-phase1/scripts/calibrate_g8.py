#!/usr/bin/env python3
"""Calibrate P1-C and P1-F T_ref values from no-fault producer runs.

P1-C uses the direct W1 producer path; P1-F uses the bounded feeder path.
Both calibrations exclude input preload and include mode preparation plus
result writeback, matching the HANDOFF T_ref definition for their path.
"""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIM_DIR = ROOT / "simulator"
SIM = SIM_DIR / "sim"
RUNS = ROOT / "runs/g8/calibration"
RESULTS = ROOT / "results/g8"
DEVICE_INI = SIM_DIR / "ini/HBM2_samsung_2M_16B_x64.ini"
SYSTEM_INI = ROOT / "configs/phase1_system_1ch.ini"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def params_text(kind: str, n_tiles: int, out: Path) -> str:
    if kind == "P1C":
        return (
            "experiment=G5\n"
            "profile=LOCAL_VISIBLE\n"
            "fault_kind=F0\n"
            f"n_tiles={n_tiles}\n"
            "seed=0\n"
            "hold_cycles=0\n"
            "consumer=HOST\n"
            "transport_delay_cycles=0\n"
            "service_interval_cycles=1\n"
            "watchdog_cycles=400000\n"
            f"device_ini={DEVICE_INI}\n"
            f"system_ini={SYSTEM_INI}\n"
            f"output_dir={out}\n"
        )
    return (
        "experiment=P1F_COST\n"
        f"n_tiles={n_tiles}\n"
        "seed=0\n"
        "consumer=HOST\n"
        "policy=EXT_OFF\n"
        "hold_cycles=0\n"
        "t_ref_cycles=0\n"
        "watchdog_cycles=400000\n"
        f"device_ini={DEVICE_INI}\n"
        f"system_ini={SYSTEM_INI}\n"
        f"output_dir={out}\n"
    )


def main() -> int:
    RUNS.mkdir(parents=True, exist_ok=True)
    RESULTS.mkdir(parents=True, exist_ok=True)
    binary_sha = sha256(SIM)
    patch_sha = sha256(ROOT / "environment/source.patch")
    rows = []
    failed = False
    for kind, filter_name, sizes in (
        ("P1C", "Phase1G5Fixture.RunCase", (1, 8)),
        ("P1F", "Phase1G8CostFixture.RunCase", (1, 8)),
    ):
        for n_tiles in sizes:
            out = RUNS / f"{kind.lower()}_nt{n_tiles}"
            out.mkdir(parents=True, exist_ok=True)
            params = out / "params.ini"
            params.write_text(params_text(kind, n_tiles, out))
            env = os.environ.copy()
            env["PHASE1_CONFIG"] = str(params)
            env["PHASE1_OUT"] = str(out)
            proc = subprocess.run(
                [str(SIM), f"--gtest_filter={filter_name}"],
                cwd=SIM_DIR,
                env=env,
                text=True,
                capture_output=True,
                timeout=180,
            )
            (out / "stdout.log").write_text(proc.stdout)
            (out / "stderr.log").write_text(proc.stderr)
            (out / "exit_code").write_text(f"{proc.returncode}\n")
            metrics = {}
            if proc.returncode == 0:
                metrics = json.loads((out / "metrics.json").read_text())
            else:
                failed = True
            rows.append({
                "kind": kind,
                "n_tiles": n_tiles,
                "exit_code": proc.returncode,
                "t_ref_cycles": metrics.get("producer_cycles"),
                "config_sha256": sha256(params),
                "binary_sha256": binary_sha,
                "patch_sha256": patch_sha,
                "run_dir": str(out.relative_to(ROOT)),
            })
            print(f"{kind}_nt{n_tiles}: exit={proc.returncode} "
                  f"t_ref={metrics.get('producer_cycles')}")
    result = {
        "schema_version": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "binary_sha256": binary_sha,
        "patch_sha256": patch_sha,
        "definition": "no-fault producer from mode preparation start to completion; "
                      "input preload excluded; mode preparation and result writeback included",
        "p1c_t_ref": {str(r["n_tiles"]): r["t_ref_cycles"] for r in rows if r["kind"] == "P1C"},
        "p1f_t_ref": {str(r["n_tiles"]): r["t_ref_cycles"] for r in rows if r["kind"] == "P1F"},
        "cases": rows,
    }
    (RESULTS / "calibration.json").write_text(json.dumps(result, indent=2) + "\n")
    print("G8_CALIBRATION=" + ("PASS" if not failed else "FAIL"))
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
