"""Preview of the locked E2 grid.

This is the M1.4 table.  It does not require G2, does not choose c or g, and
does not replace the M2 scan.  Each cell uses the existing ``run_trial``.
"""
from __future__ import annotations

import csv
from concurrent.futures import ProcessPoolExecutor
from itertools import product
from pathlib import Path
from typing import Optional

from phase3_exp.mc.mc_sim import HC_MIN, rates, run_trial

NS = (16, 32, 64, 128, 256, 16384)
CS = (1.5, 2.0, 2.5, 3.0)
GS = (1.1, 1.3, 1.5)
ATTACKS = ("A1", "A2", "A3", "A4")
SCHEMES = ("FIX", "ADA", "OURS")
TRIALS = 4

COLUMNS = (
    "n", "c", "g", "attack", "scheme", "r0", "r1", "r_start", "trials",
    "mttf_flip_mean", "mttf_sdc_mean", "censored_flip", "censored_sdc",
)


def _cell(spec: tuple) -> dict:
    n, c, g, attack, scheme = spec
    r0, r1 = rates(HC_MIN, g, c, n)
    start = r1 if scheme == "FIX" else r0
    rows = [
        run_trial(i, n=n, r=start, attack=attack, scheme=scheme, r1=r1)
        for i in range(TRIALS)
    ]
    return {
        "n": n,
        "c": c,
        "g": g,
        "attack": attack,
        "scheme": scheme,
        "r0": r0,
        "r1": r1,
        "r_start": start,
        "trials": TRIALS,
        "mttf_flip_mean": sum(row["mttf_flip"] for row in rows) / TRIALS,
        "mttf_sdc_mean": sum(row["mttf_sdc"] for row in rows) / TRIALS,
        "censored_flip": sum(row["censored_flip"] for row in rows),
        "censored_sdc": sum(row["censored_sdc"] for row in rows),
    }


def write_preliminary(path: Path, workers: Optional[int] = None) -> list:
    specs = list(product(NS, CS, GS, ATTACKS, SCHEMES))
    with ProcessPoolExecutor(max_workers=workers) as pool:
        rows = list(pool.map(_cell, specs, chunksize=4))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
    return rows


def main() -> None:
    destination = Path(__file__).resolve().parent / "e2_preliminary.csv"
    rows = write_preliminary(destination)
    print(f"wrote {destination} cells={len(rows)} trials={TRIALS}")


if __name__ == "__main__":
    main()
