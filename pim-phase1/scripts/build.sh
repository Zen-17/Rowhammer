#!/usr/bin/env bash
# G0 build: always builds the fixed upstream simulator with data storage
# enabled.  NO_STORAGE=0 is the upstream switch that ENABLES storage: the
# Sconstruct only prepends -DNO_STORAGE when the argument is non-zero.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIM_DIR="${PROJECT_ROOT}/simulator"
LOG_DIR="${PROJECT_ROOT}/logs"
UPSTREAM_COMMIT="3703d1f19c8f027360cc33a3243eb271e3bb6898"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG="${LOG_DIR}/build_${STAMP}.log"

CLEAN=0
case "${1:-}" in
    "") ;;
    --clean) CLEAN=1 ;;
    *) echo "usage: $0 [--clean]" >&2; exit 64 ;;
esac

mkdir -p "${LOG_DIR}" "${PROJECT_ROOT}/runs/baseline"
exec >> "${LOG}" 2>&1
trap 'rc=$?; echo "EXIT_CODE=${rc}"; exit ${rc}' EXIT

echo "build_start_utc=${STAMP}"
echo "+ git -C simulator rev-parse HEAD"
actual="$(git -C "${SIM_DIR}" rev-parse HEAD)"
echo "${actual}"
[[ "${actual}" == "${UPSTREAM_COMMIT}" ]] || {
    echo "wrong upstream commit: ${actual}" >&2
    exit 2
}

cd "${SIM_DIR}"
if [[ "${CLEAN}" == "1" ]]; then
    echo "+ scons -c NO_STORAGE=0 NO_LIBRARY=1"
    scons -c NO_STORAGE=0 NO_LIBRARY=1
fi
echo "+ scons NO_STORAGE=0 NO_LIBRARY=1"
scons NO_STORAGE=0 NO_LIBRARY=1

[[ -x "${SIM_DIR}/sim" ]] || {
    echo "binary not produced: ${SIM_DIR}/sim" >&2
    exit 3
}

if grep -q -- '-DNO_STORAGE' "${LOG}"; then
    echo "ERROR: build log contains -DNO_STORAGE; storage is disabled" >&2
    exit 4
fi

cd "${PROJECT_ROOT}"
echo "+ sha256sum simulator/sim"
sha256sum "${SIM_DIR}/sim" | tee "${PROJECT_ROOT}/environment/binary.sha256"
cp "${PROJECT_ROOT}/environment/binary.sha256" "${PROJECT_ROOT}/runs/baseline/binary.sha256"

{
    echo "# G0 build flags (actual command)"
    echo "command: scons NO_STORAGE=0 NO_LIBRARY=1"
    echo "working_directory: simulator"
    echo "upstream_commit: ${UPSTREAM_COMMIT}"
    echo "CXXFLAGS: -g -O2 -std=c++14 -Wall -Wno-reorder -Wno-sign-compare"
    echo "LINKFLAGS: -g -O2 -std=c++14 -Wall -Wno-reorder -Wno-sign-compare"
    echo "storage_semantics: data storage ENABLED; -DNO_STORAGE is NOT defined"
    echo "storage_evidence: Sconstruct appends -DNO_STORAGE only for non-zero NO_STORAGE;"
    echo "                  this build log contains no -DNO_STORAGE compile option."
} > "${PROJECT_ROOT}/environment/build_flags.txt"

"${PROJECT_ROOT}/.venv/bin/python" - "${PROJECT_ROOT}" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
binary = root / "simulator/sim"
record = {
    "path": "simulator/sim",
    "bytes": binary.stat().st_size,
    "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
    "build_command": "scons NO_STORAGE=0 NO_LIBRARY=1",
    "storage_enabled": True,
}
(root / "environment/binary.json").write_text(json.dumps(record, indent=2) + "\n")
(root / "runs/baseline/binary.json").write_text(json.dumps(record, indent=2) + "\n")
print(json.dumps(record, indent=2))
PY

echo "BUILD_OK"
