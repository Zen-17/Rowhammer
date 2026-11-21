#!/usr/bin/env bash
# Build the Phase 2 simulator from the shared Phase 1 source tree plus the
# Phase 2 overlay.  This is the only step that needs the Phase 1 checkout.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
P1="${P1:-$(cd "$ROOT/../pim-phase1" && pwd)}"
BUILD="$ROOT/build/simulator"
JOBS="${JOBS:-$(nproc)}"

test -f "$P1/simulator/Sconstruct" || {
    echo "missing Phase 1 simulator at $P1/simulator" >&2
    exit 1
}
test -d "$ROOT/overlay/src" || {
    echo "missing Phase 2 overlay at $ROOT/overlay/src" >&2
    exit 1
}

rm -rf "$BUILD"
mkdir -p "$BUILD"
tar -C "$P1/simulator" --exclude=bin --exclude=sim --exclude=dump -cf - . \
    | tar -C "$BUILD" -xf -
cp -a "$ROOT/overlay/src/." "$BUILD/src/"
ln -sfn build/simulator "$ROOT/simulator"

(cd "$BUILD" && scons NO_STORAGE=0 NO_LIBRARY=1 -j"$JOBS")
test -x "$ROOT/simulator/sim"
echo "PHASE2_BUILD_OK $ROOT/simulator/sim"
