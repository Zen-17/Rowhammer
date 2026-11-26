#!/usr/bin/env python3
"""Install the Stage 4–5 simulator patches that lived only in the build tree.

build_phase3.sh deletes phase3_exp/build/.  Cursor agents later patched that
generated copy (moveCodeword, identityBank, mapping_consistent, STALE_EPOCH,
observeAccess/completeObserved, phase3_operand_denied, epoch-bound requests)
without writing the same edits back into pim-phase1, pim-phase2, or the M2
hook script.  Copying this overlay after the other hooks is what keeps a
rebuild from dropping those symbols.
"""
from __future__ import annotations

import sys
from pathlib import Path

OVERLAY = Path(__file__).resolve().parent / "stage4_overlay"
FILES = (
    "Bank.cpp",
    "Bank.h",
    "BusPacket.h",
    "MemoryController.cpp",
    "MemorySystem.cpp",
    "PIMRank.cpp",
    "PIMRank.h",
    "Phase1Feeder.cpp",
    "Phase1Feeder.h",
    "Phase1Reports.cpp",
    "Phase1Reports.h",
    "Phase1Storage.cpp",
    "Phase1Storage.h",
    "Phase2Engine.cpp",
    "Phase2Ledger.cpp",
    "Phase2Ledger.h",
    "Phase2Types.cpp",
    "Phase2Types.h",
    "Transaction.h",
)
REQUIRED = (
    "moveCodeword",
    "identityBank",
    "mapping_consistent",
    "STALE_EPOCH",
    "observeAccess",
    "completeObserved",
    "phase3_operand_denied",
    "phase3PhysicalRow",
)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: apply_stage4_hooks.py BUILD_ROOT")
    build = Path(sys.argv[1])
    src = build / "src"
    missing = [name for name in FILES if not (OVERLAY / name).is_file()]
    if missing:
        raise SystemExit("stage4 overlay missing: " + ", ".join(missing))
    for name in FILES:
        target = src / name
        if not target.is_file():
            raise SystemExit("build tree missing " + name)
        target.write_text((OVERLAY / name).read_text())
    joined = "\n".join((src / name).read_text() for name in FILES)
    absent = [symbol for symbol in REQUIRED if symbol not in joined]
    if absent:
        raise SystemExit("stage4 overlay did not install: " + ", ".join(absent))
    print("PHASE3_STAGE4_HOOKS_OK", src)


if __name__ == "__main__":
    main()
