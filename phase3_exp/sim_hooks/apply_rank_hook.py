#!/usr/bin/env python3
"""Insert the read-only observer calls into a build-tree copy of Rank.cpp."""
from __future__ import annotations

import sys
from pathlib import Path

INCLUDE = '#include "ActivationObserver.h"\n'
STATE_ANCHOR = "void Rank::updateState(BusPacket* packet)\n{\n"
UPDATE_ANCHOR = "void Rank::update()\n{\n"
STATE_CALL = "    phase3ObserveCommand(this, packet);\n"
UPDATE_CALL = "    phase3ObserveCycle(this);\n"


def patch(text: str) -> str:
    if "phase3ObserveCommand" in text and "phase3ObserveCycle" in text:
        return text
    if '#include "Rank.h"\n' not in text:
        raise SystemExit("Rank.cpp is missing its header include")
    text = text.replace('#include "Rank.h"\n', '#include "Rank.h"\n' + INCLUDE, 1)
    if STATE_ANCHOR not in text or UPDATE_ANCHOR not in text:
        raise SystemExit("Rank.cpp anchors moved; refusing to guess a hook site")
    text = text.replace(STATE_ANCHOR, STATE_ANCHOR + STATE_CALL, 1)
    text = text.replace(UPDATE_ANCHOR, UPDATE_ANCHOR + UPDATE_CALL, 1)
    return text


def main() -> None:
    path = Path(sys.argv[1])
    path.write_text(patch(path.read_text()))


if __name__ == "__main__":
    main()
