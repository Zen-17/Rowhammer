#!/usr/bin/env python3
"""Diagnostic A3 run for one victim-row refresh period. Does not replace E3."""
from __future__ import annotations

import csv
import json
import os
import subprocess
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXP = ROOT / "phase3_exp"
SIM = EXP / "build/simulator/sim"
DEVICE = ROOT / "pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini"
SYSTEM = EXP / "configs/system_1ch_quiet.ini"
OUT = EXP / "results/m2r_a3_horizon"
RUNS = EXP / "runs/M2R_a3_horizon"
SEED = 20261122


def run_one(scheme: str) -> dict:
    out = RUNS / f"W2_{scheme}_A3_horizon"
    out.mkdir(parents=True, exist_ok=True)
    result = out / "result.json"
    if result.exists():
        try:
            existing = json.loads(result.read_text())
            if existing.get("status") == "SUCCESS" and int(existing.get("window_refreshes") or 0) >= 8192:
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
        "PHASE3_SCHEME": scheme,
        "PHASE3_ATTACK": "A3",
        "PHASE3_SCALE": "1.000000",
        "PHASE3_HC": "14531",
        "PHASE3_INDEX": "0",
        "PHASE3_PARA_P": "0",
        "PHASE3_SEED": str(SEED),
        "PHASE3_REFRESH": "on",
        "PHASE3_ACCESS": "reuse",
        "PHASE3_REPEAT": "1",
        "PHASE3_A3_HORIZON": "victim",
        "PHASE3_SWAP_ACCOUNTING": "model",
    })
    with (out / "gtest.log").open("w") as handle:
        completed = subprocess.run(
            [str(SIM), "--gtest_filter=Phase3M2.Cell"],
            cwd=str(out), env=env, stdout=handle, stderr=subprocess.STDOUT,
        )
    if not result.exists():
        return {"status": "FAILED", "error": f"exit {completed.returncode}", "scheme": scheme}
    return json.loads(result.read_text())


def main() -> None:
    found = []
    with ProcessPoolExecutor(max_workers=4) as pool:
        futures = [pool.submit(run_one, scheme) for scheme in ("NONE", "FIX", "ADA", "OURS")]
        for future in as_completed(futures):
            row = future.result()
            found.append(row)
            print(
                f"{row.get('status')} {row.get('scheme')} inj={row.get('injections')} "
                f"win_inj={row.get('window_injections')} win_peak={row.get('window_peak_ratio')} "
                f"win_act={row.get('window_acts')} win_neigh={row.get('window_neighbor_acts')} "
                f"win_ref={row.get('window_refreshes')} attacks={row.get('attack_transactions')}",
                flush=True,
            )
    OUT.mkdir(parents=True, exist_ok=True)
    fields = [
        "scheme", "attack", "seed", "status", "cycles", "attack_transactions", "attack_acts",
        "neighbor_acts", "refresh_commands", "victim_refreshes", "injections", "first_crossings",
        "residual_flips", "peak_ratio", "swaps", "swap_acts", "swap_clears", "swap_clears_bank0",
        "window_attacks", "window_acts", "window_neighbor_acts", "window_injections",
        "window_refreshes", "window_peak_ratio", "max_refresh_wait",
    ]
    with (OUT / "A3_horizon.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(sorted(found, key=lambda row: str(row.get("scheme"))))
    print("A3_HORIZON_DONE", flush=True)


if __name__ == "__main__":
    main()
