"""SRAM cost script.  M4 only."""
from __future__ import annotations

from phase3_exp.harness.gate import require


def main() -> None:
    require("M4")
    raise SystemExit("SRAM cost is not run in M0")


if __name__ == "__main__":
    main()
