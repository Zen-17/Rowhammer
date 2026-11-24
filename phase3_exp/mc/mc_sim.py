"""Window-level Monte Carlo for swap period, refresh, and double-sided disturbance.

This is an abstraction of the locked fault and prevention formulas.  It is not
a trace from PIMSimulator.  Time is counted in attacker activations.  A row is
refreshed every ``8192 * floor((tREFI - tRFC) / tRC)`` activations, because each
REF retires two of the 16,384 rows and the bank can issue
``floor((3900 - 350) / 47) = 75`` activations between REF commands.

Swap migration walks the region pointer.  Moving a physical row clears its own
disturbance after adding two activations to each physical neighbor.
"""
from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Dict, Optional, Tuple

import numpy as np

from phase3_exp.harness.seeds import STREAMS, mc_seed, named_streams

HC_MIN = 14531
MEDIAN_FACTOR = 1.35
SIGMA = 0.25
N_ROWS = 16384
T_RC = 47
T_REFI = 3900
T_RFC = 350
ACTS_PER_REF = (T_REFI - T_RFC) // T_RC  # 75
REFS_PER_ROW = N_ROWS // 2  # 8192
REFRESH_ACTS = REFS_PER_ROW * ACTS_PER_REF  # 614400
E_DET = 1024
F_TH = 1.0 / 16.0
ALARM = int(E_DET * F_TH)  # 64; alarm when a count is strictly greater
M3 = 1.19 + 0.5 * (1.41 - 1.19)  # linear interpolation between k=2 and k=4


def rates(hc_min: float, g: float, c: float, n: int, e_det: int = E_DET, f_th: float = F_TH) -> Tuple[int, int]:
    t_safe = hc_min / g
    r1 = max(1, int(math.floor(t_safe / (c * n))))
    r0 = max(1, int(math.floor((t_safe / c - e_det) / (f_th * n))))
    return r0, r1


def _sample_hc(rng: np.random.Generator, n: int) -> np.ndarray:
    mu = math.log(MEDIAN_FACTOR * HC_MIN)
    draws = np.empty(n, dtype=float)
    filled = 0
    while filled < n:
        batch = rng.lognormal(mu, SIGMA, size=max(n - filled, 8))
        keep = batch[batch >= HC_MIN]
        take = min(len(keep), n - filled)
        if take:
            draws[filled:filled + take] = keep[:take]
            filled += take
    return draws


def _aggressor(attack: str, step: int, victim: int, ptr: int, n: int) -> int:
    if attack == "A1":
        return victim - 1 if step % 2 == 0 else victim + 1
    if attack == "A2":
        pair = step % 8
        side = step // 8 % 2
        base = (victim + pair) % n
        return base - 1 if side == 0 else base + 1
    if attack == "A3":
        # 17 aggressors.  Two of them are the victim's neighbors, so the
        # victim is still disturbed, and each aggressor stays under 1/16.
        slot = step % 17
        if slot == 0:
            return victim - 1
        if slot == 1:
            return victim + 1
        return (victim + 2 + slot) % n
    if attack == "A4":
        return ptr - 1 if step % 2 == 0 else ptr + 1
    raise ValueError(f"unsupported attack {attack}")


def aggressor_share(attack: str, window: int = E_DET) -> float:
    """Hottest aggressor count divided by ``window``.  Used to check A3 < f_th."""
    counts: Dict[int, int] = {}
    for step in range(window):
        row = _aggressor(attack, step, victim=100, ptr=3, n=256)
        counts[row] = counts.get(row, 0) + 1
    return max(counts.values()) / window


def _disturbs_victim(aggressor: int, victim: int) -> bool:
    return abs(aggressor - victim) == 1


def run_trial(
    trial: int,
    *,
    n: int,
    r: int,
    attack: str,
    scheme: str,
    r1: int,
    horizon: int = REFRESH_ACTS,
) -> Dict[str, int]:
    streams = named_streams(mc_seed(trial))
    # Touch every stream so a later consumer cannot shift the others.
    for name in STREAMS:
        streams[name].random()
    hc = _sample_hc(streams["hc_first"], n)
    alpha = streams["vrd"].uniform(1.0, 3.5, size=n)
    victim = int(streams["flip_pos"].integers(0, n))
    ptr0 = int(streams["flip_pos"].integers(0, n))
    streams["para"].random()

    thresh1 = 2.0 * float(alpha[victim]) * float(hc[victim])
    thresh3 = M3 * thresh1
    # A swap epoch plus neighbor extras cannot reach the floor threshold.
    if n * r + 2 * (n - 1) < 2.0 * HC_MIN and REFRESH_ACTS >= 2.0 * HC_MIN:
        return {
            "mttf_flip": horizon, "mttf_sdc": horizon,
            "censored_flip": 1, "censored_sdc": 1, "escalated": 0,
        }

    disturbance = 0.0
    ptr = ptr0
    acts = 0
    since_swap = 0
    next_refresh = REFRESH_ACTS
    window_counts: Dict[int, int] = {}
    window_acts = 0
    level = "L0"
    current_r = r
    escalated = 0
    flip_at: Optional[int] = None
    sdc_at: Optional[int] = None

    def alarm() -> None:
        nonlocal level, current_r, escalated
        if scheme == "FIX":
            return
        if window_counts and max(window_counts.values()) > ALARM:
            if level == "L0":
                escalated = 1
            level = "L1"
            current_r = r1

    def swap_once() -> None:
        nonlocal disturbance, ptr, flip_at, sdc_at
        if abs(ptr - victim) == 1:
            disturbance += 2.0
            if flip_at is None and disturbance >= thresh1:
                flip_at = acts
            if sdc_at is None and disturbance >= thresh3:
                sdc_at = acts
        if ptr == victim:
            disturbance = 0.0
        ptr = (ptr + 1) % n

    while acts < horizon and (flip_at is None or sdc_at is None):
        row = _aggressor(attack, acts, victim, ptr, n)
        if _disturbs_victim(row, victim):
            disturbance += 1.0
        # An activation of the victim itself clears its disturbance.
        if row == victim:
            disturbance = 0.0
        window_counts[row] = window_counts.get(row, 0) + 1
        acts += 1
        since_swap += 1
        window_acts += 1
        if flip_at is None and disturbance >= thresh1:
            flip_at = acts
            if scheme == "OURS" and level == "L0":
                level = "L1"
                current_r = r1
                escalated = 1
        if sdc_at is None and disturbance >= thresh3:
            sdc_at = acts
        if window_acts == E_DET:
            alarm()
            window_counts = {}
            window_acts = 0
        if acts == next_refresh:
            disturbance = 0.0
            next_refresh += REFRESH_ACTS
        # After L0->L1 the period shrinks.  A counter that already passed the
        # new period still owes those swaps; equality would skip them forever.
        while since_swap >= current_r and (flip_at is None or sdc_at is None):
            swap_once()
            since_swap -= current_r
        if flip_at is not None and sdc_at is not None:
            break
        # A fresh epoch cannot reach either threshold, and refresh only clears D.
        # Escalation shortens the period, so this bound stays valid.
        if (disturbance == 0.0
                and n * current_r + 2 * (n - 1) < thresh1
                and n * current_r + 2 * (n - 1) < thresh3):
            break
    return {
        "mttf_flip": horizon if flip_at is None else flip_at,
        "mttf_sdc": horizon if sdc_at is None else sdc_at,
        "censored_flip": 1 if flip_at is None else 0,
        "censored_sdc": 1 if sdc_at is None else 0,
        "escalated": escalated,
    }


def mean_mttf(trials: int, **kwargs) -> Dict[str, float]:
    rows = [run_trial(i, **kwargs) for i in range(trials)]
    return {
        "mttf_flip_mean": float(np.mean([row["mttf_flip"] for row in rows])),
        "mttf_sdc_mean": float(np.mean([row["mttf_sdc"] for row in rows])),
        "censored_flip": int(sum(row["censored_flip"] for row in rows)),
        "rows": rows,
    }


def smoke(trials: int = 1000, monotonicity_trials: int = 200) -> dict:
    n = 64
    g = 1.5
    c = 2.0
    r0, r1 = rates(HC_MIN, g, c, n)
    if (r0, r1) != (954, 75):
        raise RuntimeError(f"locked rates drifted: r0={r0} r1={r1}")
    base = dict(n=n, r=r1, attack="A1", scheme="FIX", r1=r1)
    first = mean_mttf(trials, **base)
    second = mean_mttf(trials, **base)
    same = first["rows"] == second["rows"]
    slow = mean_mttf(monotonicity_trials, n=n, r=20000, attack="A1", scheme="FIX", r1=20000)
    fast = mean_mttf(monotonicity_trials, n=n, r=r1, attack="A1", scheme="FIX", r1=r1)
    monotonic = fast["mttf_flip_mean"] >= slow["mttf_flip_mean"]
    if not monotonic:
        raise RuntimeError("lowering r1 decreased MTTF_flip")
    if aggressor_share("A3") >= F_TH:
        raise RuntimeError("A3 aggressor share is not under f_th")
    result = {
        "model": "window_level_mc_abstraction",
        "trials": trials,
        "scheme": "FIX",
        "attack": "A1",
        "n": n,
        "g": g,
        "c": c,
        "r0": r0,
        "r1": r1,
        "seed0": mc_seed(0),
        "mttf_flip_mean": first["mttf_flip_mean"],
        "mttf_sdc_mean": first["mttf_sdc_mean"],
        "censored_flip": first["censored_flip"],
        "reproducible": same,
        "monotonic": monotonic,
        "mttf_flip_mean_r1_75": fast["mttf_flip_mean"],
        "mttf_flip_mean_r1_20000": slow["mttf_flip_mean"],
        "a3_max_share": aggressor_share("A3"),
        "acts_per_ref": ACTS_PER_REF,
        "refresh_acts": REFRESH_ACTS,
    }
    if not same:
        raise RuntimeError("Monte Carlo streams diverged across two runs")
    return result


def write_smoke(path: Path, trials: int = 1000) -> dict:
    result = smoke(trials=trials)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result
