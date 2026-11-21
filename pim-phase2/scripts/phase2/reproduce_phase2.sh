#!/usr/bin/env bash
# One-command reproduction of the Phase 2 pipeline.  Default: v2.
# Use --version v21 for the v2.1 pipeline.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
P1="${P1:-$(cd "$ROOT/../pim-phase1" && pwd)}"
cd "$ROOT"
JOBS="${JOBS:-$(nproc)}"
PY="$ROOT/.venv/bin/python"
VERSION="v2"
if [[ "${1:-}" == "--version" ]]; then
    VERSION="${2:-v2}"
fi

if [[ "$VERSION" == "v21" ]]; then
    mkdir -p logs/phase2/v21 runs/phase2/v21/l3 results/phase2/v21/l1 \
        results/phase2/v21/l2 results/phase2/v21/figure_data figures/phase2/v21
    echo "[v21 F0] preflight is documented in docs/phase2_v21_preflight.md"
    echo "[v21 F1] build and Python/C++ engine tests"
    bash scripts/phase2/build_phase2.sh | tee logs/phase2/v21/build.log
    "$PY" -m pytest tests/phase2/test_engine.py -q | tee logs/phase2/v21/pytest_engine.log
    (cd simulator && PHASE2_ADDRESS_OUT="$ROOT/results/phase2/v21/l1/address_vectors.csv" \
        ./sim --gtest_filter='Phase2Engine.*:Phase2AddressVectors.*' \
        | tee "$ROOT/logs/phase2/v21/cpp_tests.log")
    "$PY" -m pytest tests/phase2/test_address_vectors.py -q | tee logs/phase2/v21/pytest_address.log

    echo "[v21 F2] E2E-0 no-fault runs for kernel timing"
    if [[ ! -f configs/phase2/v21/calibration.json ]]; then
        cp configs/phase2/calibration.json configs/phase2/v21/calibration.json
    fi
    "$PY" scripts/phase2/v21_expand_l3.py | tee logs/phase2/v21/expand_l3.log
    "$PY" scripts/phase2/v21_run_l3.py --limit 12 --jobs "$JOBS" --resume \
        | tee logs/phase2/v21/run_l3_e2e0.log
    "$PY" scripts/phase2/v21_calibrate.py | tee logs/phase2/v21/calibrate.log
    "$PY" scripts/phase2/v21_expand_l3.py | tee logs/phase2/v21/expand_l3_final.log

    echo "[v21 F3] L2 sweep"
    "$PY" scripts/phase2/v21_run_l2.py --jobs "$JOBS" | tee logs/phase2/v21/run_l2.log

    echo "[v21 F4] full L3"
    "$PY" scripts/phase2/v21_run_l3.py --jobs "$JOBS" --resume \
        | tee logs/phase2/v21/run_l3.log
    "$PY" scripts/phase2/v21_validate_e2e.py | tee logs/phase2/v21/validate_e2e.log
    "$PY" scripts/phase2/v21_counterfactual.py | tee logs/phase2/v21/counterfactual.log

    echo "[v21 F5] F6 validation"
    "$PY" scripts/phase2/v21_f6.py | tee logs/phase2/v21/f6.log

    echo "[v21 F6] aggregate and figures"
    "$PY" scripts/phase2/v21_aggregate.py | tee logs/phase2/v21/aggregate.log
    "$PY" scripts/phase2/v21_plot.py | tee logs/phase2/v21/plot.log

    echo "[v21 F7] regression and final validation"
    "$PY" scripts/phase2/relocate_manifest.py \
        --source configs/generated/phase2/regression-manifest-after.json \
        --run-root runs/phase2/v21/regress_after \
        --out-manifest configs/generated/phase2/v21/regression-manifest-after.json \
        | tee logs/phase2/v21/relocate_after.log
    "$PY" scripts/run_suite.py \
        --manifest configs/generated/phase2/v21/regression-manifest-after.json \
        --results-dir results/phase2/v21/regress_after \
        --runs-dir runs/phase2/v21/regress_after --jobs "$JOBS" --resume \
        | tee logs/phase2/v21/regress_after.log
    "$PY" scripts/phase2/compare_runs.py \
        --before "$ROOT/runs/phase2/regress_before" \
        --after "$ROOT/runs/phase2/v21/regress_after" \
        --out "$ROOT/results/phase2/v21/regress_after/compare.json" \
        | tee logs/phase2/v21/compare.log
    "$PY" scripts/phase2/v21_build_validation.py | tee logs/phase2/v21/validation.log
    echo "PHASE2_V21_REPRODUCE=DONE"
    exit 0
fi

mkdir -p logs/phase2 runs/phase2/stage0 results/phase2/l1

echo "[Stage 0] environment and baseline"
bash scripts/phase2/build_phase2.sh | tee logs/phase2/stage0_build.log
{
    echo "# Phase 2 Stage 0 environment"
    echo "date_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "P1=$P1"
    echo "P2=$ROOT"
    echo "uname=$(uname -a)"
    echo "gcc=$(gcc --version | head -1)"
    echo "scons=$(scons --version 2>/dev/null | head -2 | tail -1)"
    echo "python=$($PY -V 2>&1)"
    echo "nproc=$(nproc)"
} > runs/phase2/stage0/env.txt
"$PY" scripts/phase2/relocate_manifest.py \
    --run-root runs/phase2/regress_before \
    --out-manifest configs/generated/phase2/regression-manifest.json \
    | tee logs/phase2/stage0_relocate.log
(cd simulator && ./sim --gtest_filter='Phase1Ecc.*:Phase1Storage.*:Phase1Runtime.*:Phase1Checkpoint.*:PIMKernelFixture.*' \
    | tee "$ROOT/logs/phase2/stage0_gtest.log")
"$PY" scripts/run_suite.py \
    --manifest configs/generated/phase2/regression-manifest.json \
    --results-dir results/phase2/regress_before \
    --runs-dir runs/phase2/regress_before --jobs "$JOBS" --resume \
    | tee logs/phase2/stage0_regress_before.log
rm -rf runs/phase2/stage0/src_snapshot
mkdir -p runs/phase2/stage0/src_snapshot
cp -a "$P1/simulator/src/." runs/phase2/stage0/src_snapshot/
(cd "$P1/simulator" && find src -type f | sort | xargs sha256sum) > runs/phase2/stage0/phase1_files.sha256
"$PY" scripts/phase2/calibrate.py | tee logs/phase2/stage0_calibrate.log

echo "[Stage 1] fact table verified in docs/phase2_mount_points.md"
test -f docs/phase2_mount_points.md

echo "[Stage 2] Python engine and L1 tests"
"$PY" -m pytest tests/phase2/test_engine.py -q | tee logs/phase2/stage2_pytest.log
bash scripts/phase2/check_isolation.sh --require-all | tee logs/phase2/stage2_isolation.log

echo "[Stage 3] L2 evaluator, statistics and first figures"
"$PY" scripts/phase2/run_l2.py --jobs "$JOBS" --resume | tee logs/phase2/stage3_run_l2.log
"$PY" scripts/phase2/aggregate.py | tee logs/phase2/stage3_aggregate.log
"$PY" scripts/phase2/plot_phase2.py | tee logs/phase2/stage3_plot.log

echo "[Stage 4] C++ engine, hooks and regression"
"$PY" scripts/phase2/relocate_manifest.py \
    --run-root runs/phase2/regress_after \
    --out-manifest configs/generated/phase2/regression-manifest-after.json \
    > logs/phase2/stage4_relocate.log
(cd simulator && PHASE2_ADDRESS_OUT="$ROOT/results/phase2/l1/address_vectors.csv" \
    ./sim --gtest_filter='Phase2Engine.*:Phase2AddressVectors.*' \
    | tee "$ROOT/logs/phase2/stage4_cpp_tests.log")
"$PY" -m pytest tests/phase2/test_address_vectors.py -q | tee logs/phase2/stage4_address.log
"$PY" scripts/run_suite.py \
    --manifest configs/generated/phase2/regression-manifest-after.json \
    --results-dir results/phase2/regress_after \
    --runs-dir runs/phase2/regress_after --jobs "$JOBS" --resume \
    | tee logs/phase2/stage4_regress_after.log
"$PY" scripts/phase2/compare_runs.py | tee logs/phase2/stage4_compare.log
(diff -ruN runs/phase2/stage0/src_snapshot simulator/src \
    > environment/phase2_source.patch) || true
bash scripts/phase2/check_isolation.sh --require-all | tee logs/phase2/stage4_isolation.log

echo "[Stage 5] L3 end-to-end"
"$PY" scripts/phase2/expand_l3.py | tee logs/phase2/stage5_expand.log
"$PY" scripts/phase2/run_l3.py --jobs "$JOBS" --resume | tee logs/phase2/stage5_run_l3.log
"$PY" scripts/phase2/validate_e2e.py | tee logs/phase2/stage5_validate.log
"$PY" -m pytest tests/phase2/test_differential.py -q | tee logs/phase2/stage5_differential.log

echo "[Stage 6] L3 sweep refit and model validation"
"$PY" scripts/phase2/refit_l3.py | tee logs/phase2/stage6_refit.log
"$PY" scripts/phase2/collect_l3_figdata.py | tee logs/phase2/stage6_l3_figdata.log
"$PY" scripts/phase2/aggregate.py | tee logs/phase2/stage6_aggregate.log
"$PY" scripts/phase2/plot_phase2.py | tee logs/phase2/stage6_plot.log

echo "[Stage 7] final figures, tables, statistics and validation"
"$PY" scripts/phase2/plot_phase2.py | tee logs/phase2/stage7_plot.log
"$PY" scripts/phase2/build_validation.py | tee logs/phase2/stage7_validation.log
echo "PHASE2_REPRODUCE=DONE"
