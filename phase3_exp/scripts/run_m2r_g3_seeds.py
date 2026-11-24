#!/usr/bin/env python3
"""Extra real-threshold REF_ON seeds for A1, A2, and A4.

Does not change the attack definition, G3, or M3. A3 stays at its locked cap.
"""
from __future__ import annotations

import csv
import hashlib
import json
import os
import subprocess
import zlib
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXP = ROOT / "phase3_exp"
SIM = EXP / "build/simulator/sim"
DEVICE = ROOT / "pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini"
SYSTEM = EXP / "configs/system_1ch_quiet.ini"
OUT = EXP / "results/m2r_g3_seeds"
RUNS = EXP / "runs/M2R_g3_seeds"
SIM_BASE_SEED = 20261122


def git_commit() -> str:
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def config_hash() -> str:
    digest = hashlib.sha256()
    for path in (EXP / "configs/design_point.json", SYSTEM, DEVICE):
        digest.update(path.read_bytes())
    return digest.hexdigest()


def run_one(spec: dict) -> dict:
    out = Path(spec["out"])
    out.mkdir(parents=True, exist_ok=True)
    result = out / "result.json"
    if result.exists():
        try:
            existing = json.loads(result.read_text())
            if existing.get("status") == "SUCCESS" and existing.get("victim_refreshes") is not None:
                existing["git_commit"] = spec["git_commit"]
                existing["config_hash"] = spec["config_hash"]
                existing["seed_index"] = spec["seed_index"]
                return existing
        except json.JSONDecodeError:
            pass
    env = os.environ.copy()
    env.update({
        "LD_LIBRARY_PATH": "/opt/data/data/anaconda3/lib",
        "PHASE3_M2_RUN": "cell",
        "PHASE3_DEVICE_INI": str(DEVICE),
        "PHASE3_SYSTEM_INI": str(SYSTEM),
        "PHASE3_OUT": str(out),
        "PHASE3_WORKLOAD": "W2",
        "PHASE3_SCHEME": spec["scheme"],
        "PHASE3_ATTACK": spec["attack"],
        "PHASE3_SCALE": "1.000000",
        "PHASE3_HC": "14531",
        "PHASE3_INDEX": str(spec["seed_index"]),
        "PHASE3_PARA_P": "0",
        "PHASE3_SEED": str(spec["seed"]),
        "PHASE3_REFRESH": "on",
        "PHASE3_ACCESS": "reuse",
        "PHASE3_REPEAT": "1",
    })
    with (out / "gtest.log").open("w") as handle:
        completed = subprocess.run(
            [str(SIM), "--gtest_filter=Phase3M2.Cell"],
            cwd=str(out), env=env, stdout=handle, stderr=subprocess.STDOUT,
        )
    if not result.exists():
        return {"status": "FAILED", "error": f"exit {completed.returncode}", **spec}
    payload = json.loads(result.read_text())
    payload["git_commit"] = spec["git_commit"]
    payload["config_hash"] = spec["config_hash"]
    payload["seed_index"] = spec["seed_index"]
    return payload


def main() -> None:
    commit = git_commit()
    hashed = config_hash()
    specs = []
    for attack in ("A1", "A2", "A4"):
        for scheme in ("NONE", "FIX", "ADA", "OURS"):
            grid = f"g3_seed|W2|{scheme}|{attack}|on|14531"
            base = zlib.crc32(grid.encode())
            for index in range(4):
                specs.append({
                    "scheme": scheme,
                    "attack": attack,
                    "seed_index": index,
                    "git_commit": commit,
                    "config_hash": hashed,
                    "seed": (SIM_BASE_SEED + base + index) % 2147483648,
                    "out": str(RUNS / f"W2_{scheme}_{attack}_real_{index}"),
                })
    found = []
    with ProcessPoolExecutor(max_workers=4) as pool:
        futures = [pool.submit(run_one, spec) for spec in specs]
        for future in as_completed(futures):
            row = future.result()
            found.append(row)
            print(
                f"{row.get('status')} {row.get('scheme')} {row.get('attack')} i={row.get('seed_index')} "
                f"inj={row.get('injections')} peak={row.get('peak_ratio')} "
                f"ref={row.get('refresh_commands')} vref={row.get('victim_refreshes')} "
                f"wait={row.get('max_refresh_wait')}",
                flush=True,
            )
    OUT.mkdir(parents=True, exist_ok=True)
    fields = [
        "git_commit", "config_hash", "seed", "seed_index", "status", "error", "workload", "scheme",
        "attack", "refresh_mode", "scale", "hc", "cycles", "attack_transactions", "refresh_commands",
        "max_refresh_wait", "victim_refreshes", "max_victim_refresh_gap", "injections",
        "first_crossings", "residual_flips", "swaps", "swap_acts", "swap_clears", "swap_clears_bank0",
        "activate_clears", "refresh_clears", "peak_ratio", "neighbor_acts",
    ]
    with (OUT / "E3_multiseed.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(sorted(found, key=lambda row: (
            str(row.get("attack")), str(row.get("scheme")), int(row.get("seed_index") or 0))))
    print("G3_SEEDS_DONE", flush=True)


if __name__ == "__main__":
    main()
