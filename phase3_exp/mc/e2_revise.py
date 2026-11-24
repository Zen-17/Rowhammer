"""Revised E2.  A censored trial is RIGHT_CENSORED, not a failure at the horizon.

The protected grid is the same n, c, g, attack, scheme set as M2.  NONE is the
negative control and does not swap.  Parameters of the locked design point are
not changed.
"""
from __future__ import annotations

import csv
import math
from concurrent.futures import ProcessPoolExecutor
from itertools import product
from pathlib import Path
from typing import Optional

from phase3_exp.harness.seeds import STREAMS, mc_seed, named_streams
from phase3_exp.harness.stats import wilson, zero_escape_upper
from phase3_exp.mc.mc_sim import (
    ALARM,
    E_DET,
    F_TH,
    HC_MIN,
    M3,
    REFRESH_ACTS,
    _aggressor,
    _disturbs_victim,
    _sample_hc,
    rates,
)

NS = (16, 32, 64, 128, 256, 16384)
CS = (1.5, 2.0, 2.5, 3.0)
GS = (1.1, 1.3, 1.5)
ATTACKS = ("A1", "A2", "A3", "A4")
SCHEMES = ("FIX", "ADA", "OURS")
HORIZON = REFRESH_ACTS


def _trial(trial: int, *, n: int, r: int, attack: str, scheme: str, r1: int,
           allow_early: bool = True, allow_fast: bool = True) -> dict:
    streams = named_streams(mc_seed(trial))
    for name in STREAMS:
        streams[name].random()
    hc = _sample_hc(streams["hc_first"], n)
    alpha = streams["vrd"].uniform(1.0, 3.5, size=n)
    victim = int(streams["flip_pos"].integers(0, n))
    ptr = int(streams["flip_pos"].integers(0, n))
    streams["para"].random()
    thresh1 = 2.0 * float(alpha[victim]) * float(hc[victim])
    thresh3 = M3 * thresh1
    bound = n * r + 2 * (n - 1)
    peak = 0.0
    swaps = 0
    if scheme != "NONE" and allow_early and bound < 2.0 * HC_MIN and HORIZON >= 2.0 * HC_MIN:
        return {
            "class": "RIGHT_CENSORED", "reason": "EPOCH_BOUND",
            "flip_at": None, "ratio": bound / thresh1, "ratio_kind": "EPOCH_UPPER",
            "swaps": HORIZON // max(r, 1), "escalated": 0,
        }
    disturbance = 0.0
    acts = 0
    since = 0
    next_refresh = HORIZON
    window = {}
    window_acts = 0
    level = "L0"
    current_r = r if scheme != "NONE" else HORIZON + 1
    escalated = 0
    flip_at = None

    def alarm() -> None:
        nonlocal level, current_r, escalated
        if scheme in ("FIX", "NONE"):
            return
        if window and max(window.values()) > ALARM:
            if level == "L0":
                escalated = 1
            level = "L1"
            current_r = r1

    def swap_once() -> None:
        nonlocal disturbance, ptr, flip_at, swaps, peak
        swaps += 1
        if abs(ptr - victim) == 1:
            disturbance += 2.0
            peak = max(peak, disturbance)
            if flip_at is None and disturbance >= thresh1:
                flip_at = acts
        if ptr == victim:
            disturbance = 0.0
        ptr = (ptr + 1) % n

    while acts < HORIZON and flip_at is None:
        row = _aggressor(attack, acts, victim, ptr, n)
        if _disturbs_victim(row, victim):
            disturbance += 1.0
        if row == victim:
            disturbance = 0.0
        peak = max(peak, disturbance)
        window[row] = window.get(row, 0) + 1
        acts += 1
        since += 1
        window_acts += 1
        if disturbance >= thresh1 and flip_at is None:
            flip_at = acts
            if scheme == "OURS" and level == "L0":
                level = "L1"
                current_r = r1
                escalated = 1
        if window_acts == E_DET:
            alarm()
            window = {}
            window_acts = 0
        if acts == next_refresh:
            disturbance = 0.0
            next_refresh += HORIZON
        if scheme != "NONE":
            while since >= current_r and flip_at is None:
                swap_once()
                since -= current_r
        if flip_at is not None:
            break
        if (allow_fast and scheme != "NONE" and disturbance == 0.0
                and n * current_r + 2 * (n - 1) < thresh1
                and n * current_r + 2 * (n - 1) < thresh3):
            remaining = HORIZON - acts
            return {
                "class": "RIGHT_CENSORED", "reason": "EPOCH_BOUND",
                "flip_at": None, "ratio": max(peak, n * current_r + 2 * (n - 1)) / thresh1,
                "ratio_kind": "EPOCH_UPPER",
                "swaps": swaps + remaining // current_r, "escalated": escalated,
            }
    if flip_at is None:
        return {
            "class": "RIGHT_CENSORED", "reason": "REFRESH_HORIZON",
            "flip_at": None, "ratio": peak / thresh1, "ratio_kind": "OBSERVED",
            "swaps": swaps, "escalated": escalated,
        }
    return {
        "class": "FLIP", "reason": "THRESHOLD",
        "flip_at": flip_at, "ratio": peak / thresh1, "ratio_kind": "OBSERVED",
        "swaps": swaps, "escalated": escalated,
    }


def _percentile(values: list, p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = p * (len(ordered) - 1)
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return ordered[lo]
    weight = rank - lo
    return ordered[lo] * (1.0 - weight) + ordered[hi] * weight


def summarize(spec: tuple) -> dict:
    n, c, g, attack, scheme, trials, commit = spec
    if scheme == "NONE":
        r0, r1, start = 0, 0, 0
    else:
        r0, r1 = rates(HC_MIN, g, c, n)
        start = r1 if scheme == "FIX" else r0
    rows = [
        _trial(i, n=n, r=start if scheme != "NONE" else 1, attack=attack, scheme=scheme, r1=max(r1, 1))
        for i in range(trials)
    ]
    flips = [row for row in rows if row["class"] == "FLIP"]
    censored = len(rows) - len(flips)
    p, low, high = wilson(len(flips), trials)
    upper = zero_escape_upper(trials) if len(flips) == 0 else ""
    ratios = [row["ratio"] for row in rows]
    kinds = {row["ratio_kind"] for row in rows}
    observed = [row["flip_at"] for row in flips]
    return {
        "git_commit": commit, "seed": mc_seed(0), "n": n, "c": c if scheme != "NONE" else "",
        "g": g if scheme != "NONE" else "", "attack": attack, "scheme": scheme,
        "f_th": F_TH, "E_det": E_DET, "r0": r0, "r1": r1, "trials": trials,
        "flips": len(flips), "right_censored": censored,
        "p_flip": p, "p_flip_low": low, "p_flip_high": high,
        "zero_failure_upper": upper,
        "mttf_observed_mean": (sum(observed) / len(observed)) if observed else "",
        "observation_limit": HORIZON,
        "max_ratio_max": max(ratios) if ratios else "",
        "max_ratio_p99": _percentile(ratios, 0.99),
        "swaps_mean": sum(row["swaps"] for row in rows) / trials,
        "ratio_kind": next(iter(kinds)) if len(kinds) == 1 else "MIXED",
        "censor_reason": rows[0]["reason"] if censored == trials else "MIXED" if censored else "NONE",
        "status": "SUCCESS",
    }


def write_security(path: Path, commit: str, trials: int = 200, workers: int = 8) -> list:
    specs = []
    for n, c, g, attack, scheme in product(NS, CS, GS, ATTACKS, SCHEMES):
        specs.append((n, c, g, attack, scheme, trials, commit))
    for n, attack in product((16, 64, 256, 16384), ATTACKS):
        specs.append((n, 0.0, 0.0, attack, "NONE", trials, commit))
    with ProcessPoolExecutor(max_workers=workers) as pool:
        rows = list(pool.map(summarize, specs, chunksize=2))
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = list(rows[0].keys())
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    return rows


def design_point_faithful(trials: int = 3) -> list:
    """Same seeds as the fast trial, but the fast path and the early return stay off."""
    out = []
    for scheme, g in (("NONE", 1.5), ("FIX", 1.5), ("ADA", 1.5), ("OURS", 1.1)):
        r0, r1 = rates(HC_MIN, g, 2.0, 64)
        start = 1 if scheme == "NONE" else (r1 if scheme == "FIX" else r0)
        for attack in ("A1", "A3"):
            rows = [
                _trial(i, n=64, r=start, attack=attack, scheme=scheme, r1=r1,
                       allow_early=False, allow_fast=False)
                for i in range(trials)
            ]
            out.append({
                "scheme": scheme, "g": g, "attack": attack, "trials": trials,
                "flips": sum(row["class"] == "FLIP" for row in rows),
                "max_ratio": max(row["ratio"] for row in rows),
            })
    return out
