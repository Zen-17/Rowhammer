# Phase 2 status

Updated after every stage. A stage is only marked PASS after its own
self-check command has been run and its numbers recorded here.

## Layout

This deliverable is a thin sibling of Phase 1:

- `$P1 = ../pim-phase1` — pure Phase 1, shared simulator sources and
  calibration inputs; no Phase 2 files remain there.
- `$P2 = .` (`pim-phase2`) — all Phase 2 code, configs, runs, results,
  figures, docs and report.
- `$P2/overlay/src` — Phase 2 C++ modules plus the four modified Phase 1 hook
  files.
- `$P2/build/simulator` — generated build tree; `$P2/simulator` is a symlink to
  it so scripts keep the `ROOT/simulator/...` convention.
- `.venv`, `environment/source.patch`, `environment/environment.json` and the
  Phase 1 INI files are symlinks into `$P1`.

| Stage | Status | Self-check | Key numbers |
|---|---|---|---|
| 0 | PASS | `relocate_manifest.py`, gtest filter, `run_suite.py`, `calibrate.py` | 240/240 SUCCESS; 11/11 gtest; calibration ±1 PASS |
| 1 | PASS | read-only source audit | 18/18 fact rows 一致; 0 差异 |
| 2 | PASS | `pytest tests/phase2/test_engine.py`; `check_isolation.sh` | 16/16; `ISOLATION=PASS` |
| 3 | PASS | `run_l2.py`; `aggregate.py`; `plot_phase2.py` | 47,946 cases; 45,360/45,360 main correct; 5 figures + T1 |
| 4 | PASS | C++ tests; address vectors; `run_suite.py` after; `compare_runs.py`; source diff | 16/16 C++ U tests; 3,000 vectors; 240/240; `REGRESSION=PASS`; only 4 whitelist files modified |
| 5 | PASS | `run_l3.py`; `validate_e2e.py`; `test_differential.py` | 745 runs; 739 SUCCESS + 6 EXPECTED_NEGATIVE; E2E 61/61; DIFF 736/736 |
| 6 | PASS | `refit_l3.py` | F6 120 rows; W1 fit MAPE 16.29%; W2 validation MAPE 22.43% |
| 7 | PASS | `build_validation.py`; final figures/report | 113/113 assertions; 6 figures + T1; `pytest tests/phase2` 753 passed |

## Stage 0 evidence

- `runs/phase2/stage0/env.txt`: kernel, gcc 11.4.0, scons 4.0.1, Python
  3.10.12, 112 CPUs, simulator SHA256.
- `configs/generated/phase2/regression-manifest.json`: 240 cases, only path
  fields rewritten; `run_id` and scientific parameters unchanged.
- `results/phase2/regress_before/summary.csv`: P1-A 144, P1-C 60, P1-E 2,
  P1-F Cost 16, P1-F Robustness 18; all SUCCESS; no FAIL/TIMEOUT.
- `runs/phase2/stage0/src_snapshot/` + `phase1_files.sha256` (89 files).
- `configs/phase2/calibration.json`: c_tile=475.1429, c_fix=328.8571,
  c_restore_per_byte=0.064819, c_restore_fix=26.0, c_quiesce=41.0; all four
  reference checks within ±1.

## Stage 1 evidence

`docs/phase2_mount_points.md` contains the 18-row verification table. No
difference from the plan's fact table was found. The mount-point list is frozen
for Stage 4's whitelist diff.

## Stage 2 evidence

- `pytest tests/phase2/test_engine.py -q` -> `16 passed` (U1–U15 plus
  `plan_whole_job` determinism).
- `bash scripts/phase2/check_isolation.sh --require-all` ->
  `ISOLATION=PASS files_checked=12`.

## Stage 3 evidence

- `scripts/phase2/run_l2.py --jobs 112`: 47,946 cases in 24 s.
- `results/phase2/l2/matrix_summary.json`: main 45,360; radius 900;
  release 2,160; status 210; ledger 36.
- `results/phase2/l2/aggregate_validation.json`: `AGGREGATE=PASS`.
- Calibration self-check (`results/phase2/l2/calibration_selfcheck.txt`):
  GLOBAL_HOLD H=0.25T 11.95% vs P1-F 10.21% (+1.74 pp);
  H=1T 47.80% vs 43.96% (+3.84 pp); both within ±5 pp.
- `figures/phase2/`: P2-F1..F5 PDF/SVG/PNG/gray + P2-T1 md/tex.

## Stage 4 evidence

- `Phase2Engine.*` + `Phase2AddressVectors.*`: 17/17 passed.
- `results/phase2/l1/address_vectors.csv`: 3,000 rows; Python cross-check
  17 passed.
- `regress_after`: 240/240 SUCCESS/EXPECTED_NEGATIVE;
  `results/phase2/regress_after/compare.json` -> `REGRESSION=PASS`.
- `environment/phase2_source.patch`: modified existing files are exactly
  `Phase1Feeder.{h,cpp}` and `tests/PIMKernel.{h,cpp}`; every other change is
  a new Phase 2 file.
- `check_isolation.sh --require-all`: 12 files, PASS.

## Stage 5 evidence

- `results/phase2/l3/summary.csv`: E2E-0 10/10, E2E-1 3 + 3 expected negative,
  E2E-2 3/3, E2E-3/4/6/7 each pass, E2E-5 1 + 1 expected negative,
  SWEEP 720/720. No FAIL/TIMEOUT/MISSING.
- `results/phase2/l3/e2e_validation.json`: 61/61 assertions.
- `results/phase2/l3/differential.json`: 736/736 decisions byte-identical.
- `pytest tests/phase2/test_differential.py -q`: 736 passed.

## Stage 6 evidence

- `configs/phase2/l3_refit.json`: c_tile=418.375, c_fix=783.0 fitted only on
  W1 E2E-0 no-fault runs.
- `results/phase2/figure_data/p2f6_validation.csv`: 120 rows (60 W1 fit,
  60 W2 validation).
- `results/phase2/l3/p2f6_summary.json`: W1 fit MAPE 16.29%, W2 validation
  MAPE 22.43%. W2 was never used in the fit.

## Stage 7 evidence

- `results/phase2/validation.json`: 113/113 assertions pass.
- `pytest tests/phase2 -q`: 753 passed (engine + address vectors + differential).
- `figures/phase2/`: P2-F1..F6 in PDF/SVG/PNG/gray PNG, `captions.md`,
  `plot_manifest.json`; P2-T1 in Markdown and LaTeX.
- `results/phase2/statistics.csv`: F1–F6 and T1 metrics with bootstrap/Wilson
  intervals.
- `report/phase2_results.md` and `scripts/phase2/reproduce_phase2.sh` are
  present.

## Open issues

None.

## v2.1 status (incremental)

| Stage | Status | Self-check | Key numbers |
|---|---|---|---|
| F0 | PASS | read-only preflight | 6/6 written in `docs/phase2_v21_preflight.md` |
| F1 | PASS | `pytest tests/phase2/test_engine.py`; L2 status | U1–U24 25/25; DELTA_FULL 30/30 SELECTIVE |
| F2 | PASS | C++ tests; `regress_after`; DIFF | C++ 26/26; 240/240 zero drift; 1,101/1,101 |
| F3 | PASS | `v21_run_l2.py`; `v21_aggregate.py` | 59,706 cases; main 45,360/45,360; release min ≥ 0 |
| F4 | PASS | `v21_run_l3.py`; `v21_validate_e2e.py` | 1,120 runs; 1,112 SUCCESS + 8 EXPECTED_NEGATIVE; E2E 91/91 |
| F5 | PASS | `v21_counterfactual.py` | 200 needed / 160 masked / 0 FAIL |
| F6 | PASS | `v21_calibrate.py`; `v21_f6.py` | kernel fits ≤ 1.35% residual; W2 overhead MAPE 11.89% |
| F7 | PASS | `v21_plot.py`; `v21_build_validation.py` | 7 figures + T1/T2; 154/154 assertions |
