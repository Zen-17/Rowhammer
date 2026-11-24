"""Wilson interval and the rule-of-three zero-escape bound.

``z`` is the playbook constant 1.959964, not the 1.96 used by the Phase 1 plotter.
"""
from __future__ import annotations

import math
from typing import Optional, Tuple

WILSON_Z = 1.959964


def wilson(k: int, n: int, z: float = WILSON_Z) -> Tuple[Optional[float], Optional[float], Optional[float]]:
    if n < 0 or k < 0 or k > n:
        raise ValueError(f"invalid binomial counts k={k} n={n}")
    if n == 0:
        return None, None, None
    p = k / n
    z2 = z * z
    denom = 1.0 + z2 / n
    center = (p + z2 / (2.0 * n)) / denom
    half = z * math.sqrt(p * (1.0 - p) / n + z2 / (4.0 * n * n)) / denom
    low = max(0.0, center - half)
    high = min(1.0, center + half)
    return p, low, high


def zero_escape_upper(n: int) -> Optional[float]:
    """Rule of three.  Defined only as the extra bound reported when k = 0."""
    if n <= 0:
        return None
    return 3.0 / n


def percentile(values: list, p: float) -> float:
    """Linear percentile on ``[0, 1]``.  An empty sample is 0."""
    if not values:
        return 0.0
    ordered = sorted(float(v) for v in values)
    if len(ordered) == 1:
        return ordered[0]
    rank = p * (len(ordered) - 1)
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    weight = rank - lo
    return ordered[lo] * (1.0 - weight) + ordered[hi] * weight
