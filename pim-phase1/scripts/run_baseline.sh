#!/usr/bin/env bash
# G0 baseline: list upstream GoogleTest cases and run the original PIM kernel
# accuracy fixture.  The default run is exactly the command required by G0.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIM_DIR="${PROJECT_ROOT}/simulator"
OUT_DIR="${PROJECT_ROOT}/runs/baseline"
LOG_DIR="${PROJECT_ROOT}/logs"
UPSTREAM_COMMIT="3703d1f19c8f027360cc33a3243eb271e3bb6898"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG="${LOG_DIR}/baseline_${STAMP}.log"

MODE="required"
case "${1:-}" in
    "") ;;
    --all) MODE="all" ;;
    *) echo "usage: $0 [--all]" >&2; exit 64 ;;
esac

mkdir -p "${OUT_DIR}" "${LOG_DIR}"
exec >> "${LOG}" 2>&1
trap 'rc=$?; echo "EXIT_CODE=${rc}"; exit ${rc}' EXIT

cd "${SIM_DIR}"
echo "baseline_start_utc=${STAMP}"
echo "+ ./sim --gtest_list_tests"
./sim --gtest_list_tests > "${OUT_DIR}/gtest-list.log" 2>&1
cat "${OUT_DIR}/gtest-list.log"

if [[ "${MODE}" == "all" ]]; then
    FILTER="*"
    XML="${OUT_DIR}/upstream-all-tests.xml"
    PREFIX="upstream-all-tests"
else
    FILTER="PIMKernelFixture.*"
    XML="${OUT_DIR}/upstream-tests.xml"
    PREFIX="upstream-tests"
fi

echo "+ ./sim --gtest_filter='${FILTER}' --gtest_output=xml:${XML}"
set +e
./sim --gtest_filter="${FILTER}" --gtest_output="xml:${XML}" \
    > "${OUT_DIR}/${PREFIX}.stdout.log" 2> "${OUT_DIR}/${PREFIX}.stderr.log"
TEST_EXIT=$?
set -e
echo "${TEST_EXIT}" > "${OUT_DIR}/${PREFIX}.exit_code"
echo "gtest_exit_code=${TEST_EXIT}"
tail -n 30 "${OUT_DIR}/${PREFIX}.stdout.log"

if [[ ! -f "${XML}" ]]; then
    echo "missing gtest XML: ${XML}" >&2
    exit 2
fi

"${PROJECT_ROOT}/.venv/bin/python" - "${PROJECT_ROOT}" "${XML}" "${TEST_EXIT}" "${MODE}" <<'PY'
import hashlib, json, pathlib, sys, xml.etree.ElementTree as ET
root, xml, test_exit, mode = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
tree = ET.parse(xml)
suite = tree.getroot()
failures = int(suite.attrib.get("failures", "0"))
errors = int(suite.attrib.get("errors", "0"))
tests = int(suite.attrib.get("tests", "0"))
cases = []
for tc in suite.iter("testcase"):
    outcome = "PASSED"
    if tc.find("failure") is not None:
        outcome = "FAILED"
    elif tc.find("skipped") is not None:
        outcome = "SKIPPED"
    cases.append({"name": f'{tc.attrib.get("classname","")}.{tc.attrib.get("name","")}',
                  "time_s": float(tc.attrib.get("time", "0")),
                  "outcome": outcome})
summary = {
    "mode": mode,
    "xml": str(xml.relative_to(root)),
    "xml_sha256": hashlib.sha256(xml.read_bytes()).hexdigest(),
    "gtest_exit_code": test_exit,
    "tests": tests,
    "failures": failures,
    "errors": errors,
    "cases": cases,
    "upstream_commit": "3703d1f19c8f027360cc33a3243eb271e3bb6898",
}
out = root / "runs/baseline" / ("upstream-tests.summary.json" if mode == "required" else "upstream-all-tests.summary.json")
out.write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
if tests == 0 or failures != 0 or errors != 0 or test_exit != 0:
    raise SystemExit(1)
PY

sha256sum "${SIM_DIR}/sim" > "${OUT_DIR}/binary.sha256"
echo "BASELINE_OK mode=${MODE}"
