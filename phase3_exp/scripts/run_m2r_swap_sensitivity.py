#!/usr/bin/env python3
"""A1 sensitivity: swap D accounting off, real ACTs only. Does not replace E3."""
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
OUT = EXP / "results/m2r_swap_sensitivity"
RUNS = EXP / "runs/M2R_swap_sensitivity"
SEEDS = (20261122, 1147387685)


def run_one(scheme: str, accounting: str, seed: int) -> dict:
    out = RUNS / f"W2_{scheme}_A1_{accounting}_{seed}"
    out.mkdir(parents=True, exist_ok=True)
    result = out / "result.json"
    if result.exists():
        try:
            existing = json.loads(result.read_text())
            if existing.get("status") == "SUCCESS" and existing.get("swap_accounting") == accounting:
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
        "PHASE3_ATTACK": "A1",
        "PHASE3_SCALE": "1.000000",
        "PHASE3_HC": "14531",
        "PHASE3_INDEX": "0",
        "PHASE3_PARA_P": "0",
        "PHASE3_SEED": str(seed),
        "PHASE3_REFRESH": "on",
        "PHASE3_ACCESS": "reuse",
        "PHASE3_REPEAT": "1",
        "PHASE3_SWAP_ACCOUNTING": accounting,
    })
    with (out / "gtest.log").open("w") as handle:
        completed = subprocess.run(
            [str(SIM), "--gtest_filter=Phase3M2.Cell"],
            cwd=str(out), env=env, stdout=handle, stderr=subprocess.STDOUT,
        )
    if not result.exists():
        return {"status": "FAILED", "error": f"exit {completed.returncode}", "scheme": scheme,
                "swap_accounting": accounting, "seed": seed}
    payload = json.loads(result.read_text())
    payload["swap_accounting"] = accounting
    return payload


def main() -> None:
    specs = [(scheme, accounting, seed) for seed in SEEDS for accounting in ("model", "commands")
             for scheme in ("NONE", "FIX", "ADA", "OURS")]
    found = []
    with ProcessPoolExecutor(max_workers=4) as pool:
        futures = [pool.submit(run_one, scheme, accounting, seed) for scheme, accounting, seed in specs]
        for future in as_completed(futures):
            row = future.result()
            found.append(row)
            print(
                f"{row.get('status')} {row.get('scheme')} {row.get('swap_accounting')} seed={row.get('seed')} "
                f"inj={row.get('injections')} peak={row.get('peak_ratio')} "
                f"swaps={row.get('swaps')} clears={row.get('swap_clears')} "
                f"bank0={row.get('swap_clears_bank0')}",
                flush=True,
            )
    OUT.mkdir(parents=True, exist_ok=True)
    fields = [
        "scheme", "attack", "seed", "swap_accounting", "status", "cycles", "attack_transactions",
        "attack_acts", "neighbor_acts", "refresh_commands", "injections", "first_crossings",
        "residual_flips", "peak_ratio", "swaps", "swap_acts", "swap_clears", "swap_clears_bank0",
        "activate_clears", "refresh_clears",
    ]
    with (OUT / "A1_swap_sensitivity.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(sorted(found, key=lambda row: (str(row.get("swap_accounting")), str(row.get("scheme")))))
    print("SWAP_SENSITIVITY_DONE", flush=True)


if __name__ == "__main__":
    main()
