#!/usr/bin/env bash
# G0 bootstrap: fetch the fixed PIMSimulator commit and verify the local
# toolchain / Python environment.  It never follows a moving branch silently.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIM_DIR="${PROJECT_ROOT}/simulator"
LOG_DIR="${PROJECT_ROOT}/logs"
UPSTREAM_URL="https://github.com/SAITPublic/PIMSimulator.git"
UPSTREAM_COMMIT="3703d1f19c8f027360cc33a3243eb271e3bb6898"
UPSTREAM_BRANCH="dev"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG="${LOG_DIR}/bootstrap_${STAMP}.log"

mkdir -p "${LOG_DIR}"
exec > >(tee -a "${LOG}") 2>&1
trap 'rc=$?; echo "EXIT_CODE=${rc}"; exit ${rc}' EXIT
set -x

MODE="full"
case "${1:-}" in
    "") ;;
    --check-only) MODE="check" ;;
    *) echo "usage: $0 [--check-only]" >&2; exit 64 ;;
esac

echo "bootstrap_start_utc=${STAMP}"
echo "project_root=${PROJECT_ROOT}"
echo "upstream_url=${UPSTREAM_URL}"
echo "upstream_commit=${UPSTREAM_COMMIT}"

# ---- tool checks ---------------------------------------------------------
for tool in git g++ scons python3; do
    command -v "${tool}" >/dev/null || {
        echo "missing tool: ${tool}" >&2
        echo "Ubuntu: apt-get install -y git build-essential scons libgtest-dev python3 python3-venv python3-pip" >&2
        exit 2
    }
done
git --version
g++ --version | head -n 1
scons --version | head -n 3
python3 --version

# ---- fixed source --------------------------------------------------------
if [[ -d "${SIM_DIR}/.git" ]]; then
    actual="$(git -C "${SIM_DIR}" rev-parse HEAD)"
    if [[ "${actual}" != "${UPSTREAM_COMMIT}" ]]; then
        echo "refusing to overwrite existing work: HEAD=${actual}, expected=${UPSTREAM_COMMIT}" >&2
        exit 3
    fi
    echo "existing simulator already at fixed commit"
else
    if [[ "${MODE}" == "check" ]]; then
        echo "simulator missing in --check-only mode" >&2
        exit 3
    fi
    if [[ -e "${SIM_DIR}" ]]; then
        echo "simulator path exists but is not a git repository: ${SIM_DIR}" >&2
        exit 3
    fi
    git clone --depth 1 --branch "${UPSTREAM_BRANCH}" "${UPSTREAM_URL}" "${SIM_DIR}"
    if ! git -C "${SIM_DIR}" cat-file -e "${UPSTREAM_COMMIT}^{commit}" 2>/dev/null; then
        # A later dev tip would not contain the fixed commit in a shallow
        # clone; fetch history rather than switching to an unknown revision.
        git -C "${SIM_DIR}" fetch --unshallow origin
    fi
    git -C "${SIM_DIR}" checkout --detach "${UPSTREAM_COMMIT}"
fi

actual="$(git -C "${SIM_DIR}" rev-parse HEAD)"
[[ "${actual}" == "${UPSTREAM_COMMIT}" ]] || {
    echo "commit verification failed: ${actual}" >&2
    exit 4
}
git -C "${SIM_DIR}" log -1 --format='%H %ci %s'
git -C "${SIM_DIR}" status --porcelain

# ---- Python environment --------------------------------------------------
VENV="${PROJECT_ROOT}/.venv"
if [[ ! -x "${VENV}/bin/python" ]]; then
    if [[ "${MODE}" == "check" ]]; then
        echo "virtualenv missing in --check-only mode" >&2
        exit 5
    fi
    python3 -m venv "${VENV}"
fi

if [[ "${MODE}" != "check" ]]; then
    if [[ -f "${PROJECT_ROOT}/environment/requirements.lock.txt" ]]; then
        "${VENV}/bin/python" -m pip install -r "${PROJECT_ROOT}/environment/requirements.lock.txt"
    else
        "${VENV}/bin/python" -m pip install numpy pandas matplotlib pyyaml jsonschema pytest
        "${VENV}/bin/python" -m pip freeze > "${PROJECT_ROOT}/environment/requirements.lock.txt"
    fi
fi

"${VENV}/bin/python" -m pip check
"${VENV}/bin/python" - <<'PY'
import numpy, pandas, matplotlib, yaml, jsonschema, pytest
print("python imports ok:",
      "numpy", numpy.__version__,
      "pandas", pandas.__version__,
      "matplotlib", matplotlib.__version__,
      "pyyaml", yaml.__version__,
      "jsonschema", jsonschema.__version__,
      "pytest", pytest.__version__)
PY

# ---- lock verification ---------------------------------------------------
if [[ -f "${PROJECT_ROOT}/environment/upstream.lock.json" ]]; then
    "${VENV}/bin/python" - "${PROJECT_ROOT}" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
lock = json.loads((root / "environment/upstream.lock.json").read_text())
sim = root / "simulator"
assert lock["repository"]["commit"] == __import__("subprocess").check_output(
    ["git", "-C", str(sim), "rev-parse", "HEAD"], text=True).strip()
for item in lock["original_configs"]:
    p = sim / item["path"]
    got = hashlib.sha256(p.read_bytes()).hexdigest()
    if got != item["sha256"]:
        raise SystemExit(f"config hash mismatch: {item['path']} {got} != {item['sha256']}")
print("upstream.lock.json verification ok")
PY
else
    echo "upstream.lock.json not present yet (first bootstrap)"
fi

echo "BOOTSTRAP_OK mode=${MODE}"
