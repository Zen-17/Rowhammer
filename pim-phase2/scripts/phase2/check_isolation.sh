#!/usr/bin/env bash
# Isolation gate for the recovery engine sources.
#
# The engine may read only: the ledger, the public report, the engine config,
# checkpoint availability and output-gate state.  Forbidden words are checked
# in the model-side and simulator-side recovery modules.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REQUIRE_ALL=0
if [[ "${1:-}" == "--require-all" ]]; then
    REQUIRE_ALL=1
fi

pattern='truth|reference|inject|golden|expected'
files=(
    "$ROOT/phase2/layout.py"
    "$ROOT/phase2/engine.py"
    "$ROOT/simulator/src/Phase2Layout.h"
    "$ROOT/simulator/src/Phase2Layout.cpp"
    "$ROOT/simulator/src/Phase2Ledger.h"
    "$ROOT/simulator/src/Phase2Ledger.cpp"
    "$ROOT/simulator/src/Phase2Engine.h"
    "$ROOT/simulator/src/Phase2Engine.cpp"
    "$ROOT/simulator/src/Phase2Gate.h"
    "$ROOT/simulator/src/Phase2Gate.cpp"
    "$ROOT/simulator/src/Phase2Isolation.h"
    "$ROOT/simulator/src/Phase2Isolation.cpp"
)

status=0
checked=0
for file in "${files[@]}"; do
    if [[ ! -f "$file" ]]; then
        if [[ "$REQUIRE_ALL" -eq 1 ]]; then
            echo "MISSING $file"
            status=1
        else
            echo "SKIP $file (not created yet)"
        fi
        continue
    fi
    checked=$((checked + 1))
    if grep -nEi "$pattern" "$file"; then
        echo "FORBIDDEN_WORD $file"
        status=1
    fi
done

if [[ "$status" -eq 0 ]]; then
    echo "ISOLATION=PASS files_checked=$checked require_all=$REQUIRE_ALL"
fi
exit "$status"
