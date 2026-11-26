#!/usr/bin/env bash
# Build a Phase 3 simulator tree.  Phase 1 and Phase 2 sources are copied, not edited.
#
# DANGER: this script starts with `rm -rf "$BUILD"`.  Stage 4–5 patches used
# to live only in that generated tree.  They are now reapplied from
# sim_hooks/stage4_overlay via apply_stage4_hooks.py.  Do not comment that
# step out, and do not patch build/simulator/src as if it were source.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
P1="${P1:-$(cd "$ROOT/../pim-phase1" && pwd)}"
P2="${P2:-$(cd "$ROOT/../pim-phase2" && pwd)}"
BUILD="$ROOT/build/simulator"
JOBS="${JOBS:-$(nproc)}"
ANACONDA="${ANACONDA:-/opt/data/data/anaconda3}"
export PYTHONPATH="$ROOT/.tools${PYTHONPATH:+:$PYTHONPATH}"
export CPATH="$ANACONDA/include${CPATH:+:$CPATH}"
export LIBRARY_PATH="$ANACONDA/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
export LD_LIBRARY_PATH="$ANACONDA/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
SCONS="${SCONS:-$ROOT/.tools/bin/scons}"

test -f "$P1/simulator/Sconstruct"
test -d "$P2/overlay/src"
test -f "$ROOT/sim_hooks/ActivationObserver.cpp"
test -f "$ROOT/sim_hooks/stage4_overlay/Phase1Storage.cpp"
test -f "$ROOT/sim_hooks/apply_stage4_hooks.py"

rm -rf "$BUILD"
mkdir -p "$BUILD"
tar -C "$P1/simulator" --exclude=bin --exclude=sim --exclude=dump -cf - . | tar -C "$BUILD" -xf -
cp -a "$P2/overlay/src/." "$BUILD/src/"
cp "$ROOT/sim_hooks/ActivationObserver.h" "$ROOT/sim_hooks/ActivationObserver.cpp" "$BUILD/src/"
cp "$ROOT/sim_hooks/ReadDisturbanceModel.h" "$ROOT/sim_hooks/ReadDisturbanceModel.cpp" "$BUILD/src/"
cp "$ROOT/sim_hooks/RegionMap.h" "$ROOT/sim_hooks/RegionMap.cpp" "$BUILD/src/"
cp "$ROOT/sim_hooks/Phase3Bridge.h" "$ROOT/sim_hooks/Phase3Bridge.cpp" "$BUILD/src/"
cp "$ROOT/sim_hooks/Phase3M0Test.cpp" "$ROOT/sim_hooks/Phase3M1Test.cpp" "$ROOT/sim_hooks/Phase3M2Test.cpp" "$ROOT/sim_hooks/Phase3M2RTest.cpp" "$BUILD/src/tests/"
"$ANACONDA/bin/python3" "$ROOT/sim_hooks/apply_rank_hook.py" "$BUILD/src/Rank.cpp"
"$ANACONDA/bin/python3" "$ROOT/sim_hooks/apply_m2_hooks.py" "$BUILD"
"$ANACONDA/bin/python3" "$ROOT/sim_hooks/apply_gemv_hook.py" "$BUILD/src/tests/PIMKernel.cpp"
"$ANACONDA/bin/python3" "$ROOT/sim_hooks/apply_stage4_hooks.py" "$BUILD"
"$ANACONDA/bin/python3" - "$BUILD/Sconstruct" "$ANACONDA" <<'PY'
import pathlib, sys
path, anaconda = sys.argv[1:]
file = pathlib.Path(path)
file.write_text(file.read_text()
    .replace('CPPPATH=[base_path["lib"], base_path["source"], base_path["tools"]]',
             'CPPPATH=[base_path["lib"], base_path["source"], base_path["tools"], "%s/include"]' % anaconda)
    .replace("LIBPATH=['.']",
             'LIBPATH=[".", "%s/lib"]' % anaconda))
PY

(cd "$BUILD" && "$SCONS" NO_STORAGE=0 NO_LIBRARY=1 -j"$JOBS")
test -x "$BUILD/sim"
echo "PHASE3_BUILD_OK $BUILD/sim"
