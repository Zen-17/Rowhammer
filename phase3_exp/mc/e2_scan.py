"""Full E2 grid.  Requires G2=GO.  Does not choose a new c or g by itself."""
from __future__ import annotations

import csv
import subprocess
from concurrent.futures import ProcessPoolExecutor
from itertools import product
from pathlib import Path
from typing import Optional

from phase3_exp.harness.gate import require
from phase3_exp.harness.paths import RESULTS
from phase3_exp.harness.seeds import MC_BASE_SEED, mc_seed
from phase3_exp.mc.mc_sim import E_DET, F_TH, HC_MIN, REFRESH_ACTS, rates, run_trial

NS = (16, 32, 64, 128, 256, 16384)
CS = (1.5, 2.0, 2.5, 3.0)
GS = (1.1, 1.3, 1.5)
ATTACKS = ("A1", "A2", "A3", "A4")
SCHEMES = ("FIX", "ADA", "OURS")
TRIALS = 1000

COLUMNS = (
    "git_commit", "seed", "n", "c", "g", "attack", "scheme", "f_th", "E_det",
    "r0", "r1", "r_start", "trials", "mttf_flip_mean", "mttf_sdc_mean",
    "censored_flip", "censored_sdc", "escalated",
)


def git_commit() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=Path(__file__).resolve().parents[2],
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def _cell(spec: tuple) -> dict:
    n, c, g, attack, scheme, commit = spec
    r0, r1 = rates(HC_MIN, g, c, n)
    start = r1 if scheme == "FIX" else r0
    rows = [
        run_trial(i, n=n, r=start, attack=attack, scheme=scheme, r1=r1)
        for i in range(TRIALS)
    ]
    return {
        "git_commit": commit,
        "seed": mc_seed(0),
        "n": n,
        "c": c,
        "g": g,
        "attack": attack,
        "scheme": scheme,
        "f_th": F_TH,
        "E_det": E_DET,
        "r0": r0,
        "r1": r1,
        "r_start": start,
        "trials": TRIALS,
        "mttf_flip_mean": sum(row["mttf_flip"] for row in rows) / TRIALS,
        "mttf_sdc_mean": sum(row["mttf_sdc"] for row in rows) / TRIALS,
        "censored_flip": sum(row["censored_flip"] for row in rows),
        "censored_sdc": sum(row["censored_sdc"] for row in rows),
        "escalated": sum(row["escalated"] for row in rows),
    }


def write_security(path: Path, workers: Optional[int] = None) -> list:
    commit = git_commit()
    specs = [
        (*spec, commit)
        for spec in product(NS, CS, GS, ATTACKS, SCHEMES)
    ]
    with ProcessPoolExecutor(max_workers=workers) as pool:
        rows = list(pool.map(_cell, specs, chunksize=2))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
    if any(any(value != value for value in row.values() if isinstance(value, float)) for row in rows):
        raise RuntimeError("E2 grid contains NaN")
    return rows


def design_point_ok(rows: list) -> tuple:
    """True only when both locked margins survive every attack to the refresh horizon."""
    horizon = float(REFRESH_ACTS)

    def covered(g: float, schemes: tuple) -> bool:
        matched = [
            row for row in rows
            if int(row["n"]) == 64 and float(row["c"]) == 2.0 and float(row["g"]) == g
            and row["scheme"] in schemes
        ]
        if len(matched) != len(schemes) * len(ATTACKS):
            return False
        return all(
            float(row["mttf_flip_mean"]) == horizon
            and float(row["mttf_sdc_mean"]) == horizon
            and int(row["censored_flip"]) == TRIALS
            and int(row["censored_sdc"]) == TRIALS
            for row in matched
        )

    ok = covered(1.5, ("FIX", "ADA")) and covered(1.1, ("OURS",))
    candidates = []
    for c, g in product(CS, GS):
        group = [
            row for row in rows
            if int(row["n"]) == 64 and float(row["c"]) == c and float(row["g"]) == g
        ]
        if group and all(
            int(row["censored_flip"]) == TRIALS and int(row["censored_sdc"]) == TRIALS
            for row in group
        ):
            candidates.append({"c": c, "g": g, "schemes": "FIX,ADA,OURS", "attacks": "A1-A4"})
    return ok, candidates


def main() -> None:
    require("M2")
    destination = RESULTS / "E2_mc_security.csv"
    rows = write_security(destination)
    ok, candidates = design_point_ok(rows)
    print(f"wrote {destination} cells={len(rows)} trials={TRIALS} design_point={'LOCK' if ok else 'STOP'}")
    print(f"seed_base={MC_BASE_SEED} candidates={candidates}")


if __name__ == "__main__":
    main()
