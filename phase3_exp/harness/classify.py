"""Map a closed-loop run onto exactly one of O1..O6.

Phase 2 decision strings are inputs.  They are not rewritten.  ``STATUS=FAILED``
is not an outcome and must not be turned into a zero.
"""
from __future__ import annotations

from typing import Optional

OUTCOMES = ("O6", "O5", "O4", "O3", "O2", "O1")

_SELECTIVE = {"SELECTIVE_RECOVERED", "SOURCE_REPAIRED_ONLY"}
_FALLBACK = {"WHOLE_JOB_FALLBACK", "BASELINE_B0"}
_WITHHELD = {"OUTSIDE_WINDOW_NO_TRUSTED_INPUT", "WITHHELD", "DETECTED_UNRECOVERABLE"}


def classify(
    *,
    status: str,
    bit_exact: bool,
    flip_count: int,
    replayed: bool,
    withheld: bool,
    recovery_failed: bool,
    phase2_status: str = "",
    latent_flips: int = 0,
) -> Optional[dict]:
    if status == "FAILED":
        return None
    if flip_count < 0 or latent_flips < 0:
        raise ValueError("flip counts must be non-negative")
    phase2 = phase2_status or ""
    withheld = withheld or phase2 in _WITHHELD
    replayed = replayed or phase2 in _SELECTIVE or phase2 in _FALLBACK
    fallback = phase2 in _FALLBACK
    selective = phase2 in _SELECTIVE

    if not bit_exact:
        outcome = "O6"
    elif withheld and recovery_failed:
        outcome = "O5"
    elif fallback:
        outcome = "O4"
    elif selective or replayed:
        outcome = "O3"
    elif flip_count > 0:
        outcome = "O2"
    else:
        outcome = "O1"
    return {"outcome": outcome, "latent_flips": int(latent_flips)}
