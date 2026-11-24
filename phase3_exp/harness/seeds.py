"""Locked seeds and independent PRNG streams.

Monte Carlo trial ``i`` uses ``MC_BASE_SEED + i``.  A simulator grid uses
``(SIM_BASE_SEED + crc32(grid_id) + i) mod 2**31``.  The four named streams are
spawned in a fixed order so that drawing from one stream never advances another.
"""
from __future__ import annotations

import zlib
from typing import Dict

import numpy as np

MC_BASE_SEED = 20260000
SIM_BASE_SEED = 20261122
STREAMS = ("hc_first", "vrd", "flip_pos", "para")


def mc_seed(trial: int) -> int:
    if trial < 0:
        raise ValueError("trial index must be non-negative")
    return MC_BASE_SEED + int(trial)


def sim_seed(grid_id: str, index: int) -> int:
    if index < 0:
        raise ValueError("run index must be non-negative")
    checksum = zlib.crc32(grid_id.encode("utf-8")) & 0xFFFFFFFF
    return (SIM_BASE_SEED + checksum + int(index)) % (2**31)


def named_streams(seed: int) -> Dict[str, np.random.Generator]:
    parent = np.random.SeedSequence([int(seed)])
    children = parent.spawn(len(STREAMS))
    return {
        name: np.random.Generator(np.random.PCG64(child))
        for name, child in zip(STREAMS, children)
    }
