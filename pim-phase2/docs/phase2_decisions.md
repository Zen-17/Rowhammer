# Phase 2 decisions

Format: number / situation / choice / reason / affected files.

## #001 — Missing system toolchain in the execution image

- **Situation**: the image had no `/usr/bin/python3` binary, no `scons`, and no
  `libgtest` headers/library, although `.venv` (Python 3.10 packages),
  `simulator/sim` and the Phase 1 results were present.
- **Choice**: install the distro packages `python3.10`, `python3.10-venv`,
  `scons`, `libgtest-dev`; restore `.venv/bin/python*` to point at the system
  CPython 3.10; keep every Python package from
  `environment/requirements.lock.txt` unchanged (numpy 2.2.6, pandas 2.3.3,
  matplotlib 3.10.9, PyYAML, pytest 9.2.0).
- **Reason**: the plan's commands require a working Python, SCons and gtest;
  installing interpreters/build tools is not a new Python dependency.
- **Affected**: `runs/phase2/stage0/env.txt`, `.venv/bin/python*`.

## #002 — W2 `Y` trusted input is a Phase 2 checkpoint extension

- **Situation**: `Phase1CheckpointStore` only stores W1 A/X. W2 fault positions
  include `Y`, which is an INITIAL input and must be restorable.
- **Choice**: Phase 2 keeps its own `CheckpointView` (operand -> tile -> byte
  availability) backed by a trusted host copy of A/X/Y created by the fixture
  before execution. Phase 1's checkpoint class is untouched and is still used
  verbatim in the B0 path.
- **Reason**: restoring `Y` is required for logical closure of W2; changing
  Phase 1's checkpoint would break the zero-change rule.
- **Affected**: `simulator/src/Phase2Checkpoint.*`, `simulator/src/tests/Phase2E2ETestCases.cpp`,
  `phase2/engine.py`.

## #003 — B0 decision representation in L3

- **Situation**: the engine's `Policy` enum contains B1/B2/B2R only, but the L3
  suite runs B0 cases and the differential test expects a `decision.json` for
  every run.
- **Choice**: B0 calls a separate `planWholeJob()` on the same ledger. Its
  decision uses `status=WHOLE_JOB_FALLBACK` with
  `fallback_reasons=["B0_BASELINE"]`, `affected` = every completed op of the
  job and `restore_list` = every INITIAL input. The Python reference engine
  implements the same function.
- **Reason**: B0 is literally the whole-job fallback path; representing it this
  way keeps one decision schema and makes the differential check meaningful.
- **Affected**: `Phase2Engine.*`, `phase2/engine.py`, `Phase2E2ETestCases.cpp`,
  `tests/phase2/test_differential.py`.

## #004 — Non-truth engine inputs for differential replay

- **Situation**: `report overflow`, `ledger_truncated`, the ACK cycle and
  checkpoint availability are engine inputs but are not derivable from
  `ledger.csv` alone.
- **Choice**: each L3 run writes `decision_inputs.json` containing only
  non-truth engine inputs (`t_ack`, `delta_cycles`, `report_overflow`,
  `ledger_truncated`, `checkpoint_missing`, `dep_complete_false_ops`,
  `gate_consumed`). `truth.csv`, `reference.bin` and the injected fault
  coordinates are never referenced.
- **Reason**: needed for an honest independent replay without leaking truth.
- **Affected**: `Phase2E2ETestCases.cpp`, `tests/phase2/test_differential.py`.

## #005 — Radius-expanded suspects get their own decision field

- **Situation**: the plan asks that `R` and the radius-expanded `R_k` be listed
  separately, but the draft `Decision` struct only has `suspect_items`.
- **Choice**: keep `suspect_items` = `R` and add
  `radius_suspect_items` = `R_k` in both engines and in JSON.
- **Reason**: preserves the distinction without changing the meaning of the
  original field; both implementations emit the same sorted order.
- **Affected**: `Phase2Types.h`, `Phase2Engine.*`, `phase2/engine.py`.

## #006 — B0 L3 metrics count the full job re-execution

- **Situation**: the engine decision for B0 lists only the completed ops in
  `affected`, while B0 physically re-executes every kernel from the start.
- **Choice**: after `planWholeJob`, the fixture executes all kernels over all
  tiles and reports `replay_tiles = kernels * n_tiles`, `replay_runs = kernels`;
  `affected_ops` still reports the decision's completed-op count.
- **Reason**: the cost/verification metrics must describe what was executed,
  not just what was known at ACK.
- **Affected**: `simulator/src/tests/Phase2E2ETestCases.cpp`,
  `scripts/phase2/validate_e2e.py`.

## #007 — L2 DEP_RELEASE may report negative overhead

- **Situation**: `DEP_RELEASE(Δ)` can publish a tile after its producer op
  completes, before the kernel-level producer barrier used by `GLOBAL_HOLD`.
- **Choice**: keep the model as specified and report negative overhead as an
  early-release benefit; it is not clipped to zero.
- **Reason**: clipping would hide a real consequence of the release rule; the
  report states explicitly that this is a model upper bound, not hardware data.
- **Affected**: `phase2/timing.py`, `figures/phase2/p2f3_release.*`,
  `report/phase2_results.md`.

## #008 — L3 refit fits W1 only and validates W2 out of sample

- **Situation**: L3 per-tile cost differs from the Phase 1 ADD calibration, and
  MUL/RELU have no W1-equivalent fit data.
- **Choice**: fit `c_tile`/`c_fix` from W1 E2E-0 no-fault kernel times only;
  predict W2 with the same ADD per-tile cost and report the out-of-sample MAPE
  (22.43%) without tuning on W2.
- **Reason**: the plan forbids fitting and validating on the same data; the
  residual is the expected model limitation for MUL/RELU.
- **Affected**: `scripts/phase2/refit_l3.py`, `configs/phase2/l3_refit.json`,
  `figures/phase2/p2f6_validation.*`.

## V21-01 — E2E-8 first late point uses 6,200 cycles, not T_ADD=4,137

- **Situation**: with `transport_delay=T_ADD` the report ACKs while the MUL kernel
  is still in its mode-switch preamble; the simulator aborts with a DRAM command
  ordering error, and MUL_2 never enters the ledger.
- **Choice**: use `transport_delay={6200,8260}` for E2E-8, i.e. T_ADD + 0.5·T_ADD
  and 2·T_ADD. Both place the report after MUL_2 has completed and before the job
  ends.
- **Reason**: the purpose of E2E-8 is to show forward dependency propagation, not
  to test a mid-preamble ACK. The 4,137 point is documented as an invalid
  simulator schedule, not as a recovery result.
- **Affected**: `configs/phase2/v21/l3_suite.yaml`, `scripts/phase2/v21_expand_l3.py`,
  `report/phase2_results_v21.md`.

## V21-02 — E2E-9 release comparison uses GLOBAL H=T and producer-only L3 time

- **Situation**: host reads cannot occur during PIM mode, and the L3 fixture does
  not include the host-read cost in `end_to_end_cycles`.
- **Choice**: GLOBAL_HOLD uses H=T (the full no-fault producer time); DEP_RELEASE
  uses Delta={0.25T,1.0T}. P2-F3 adds the calibrated host-read cost to the L3
  points so they share the L2 host-inclusive basis; the raw L3 metrics remain
  producer-only.
- **Reason**: this gives a conservative GLOBAL baseline and makes the
  DEP <= GLOBAL assertion meaningful without mixing two time bases.
- **Affected**: `scripts/phase2/v21_expand_l3.py`, `scripts/phase2/v21_plot.py`.

## V21-03 — Empty `R_read` falls back to the whole spatial R

- **Situation**: a report can come from a read that the ledger did not record.
- **Choice**: if `R_read` is empty, keep the full `R` and add
  `UNLOGGED_READER` to `fallback_reasons`, which triggers whole-job fallback.
- **Reason**: conservative correctness; silently shrinking R in this case could
  miss the corrupt item.
- **Affected**: `Phase2Engine.*`, `phase2/engine.py`, U22.

## V21-04 — Replay coalescing is unit-tested but does not trigger in the matrix

- **Situation**: with c_fix=789 and c_tile=418 (W1), only a one-tile gap is
  cheaper to replay than to pay another fixed cost. The measured affected sets
  are contiguous.
- **Choice**: implement and test U23/U24; report `coalesced_extra_tiles` but do
  not claim a runtime benefit. No synthetic gaps were inserted into the main
  matrix.
- **Reason**: inserting artificial gaps would change the workload/fault model.
- **Affected**: `Phase2Engine.*`, `phase2/engine.py`, P2-F1/P2-F2.

## V21-05 — P2-F6 validation excludes NO_REPLAY controls

- **Situation**: NO_REPLAY runs deliberately skip recovery, so their
  `recovery_cycles_excl_resume` is not a recovery prediction target.
- **Choice**: validation set = all normal W1/W2 fault-recovery runs; NO_REPLAY
  controls are used only by the F2 counterfactual.
- **Reason**: comparing a recovery model with an intentionally disabled recovery
  would inflate MAPE without scientific meaning.
- **Affected**: `scripts/phase2/v21_f6.py`, P2-F6.

## V21-06 — W4 K2 uses the MUL fit as an ASSUMED cost

- **Situation**: W4 has no hardware/kernel timing of its own.
- **Choice**: use the W2 MUL `c_fix`/`c_tile` for W4 K2 and mark it ASSUMED in
  the figure caption and report.
- **Reason**: W4 is a dependency-propagation workload, not a cost-measurement
  workload.
- **Affected**: `configs/phase2/v21/calibration.json`, P2-F7.

## V21-07 — `BASELINE_B0` supersedes decision #003

- **Situation**: B0 is a baseline, not a failure status.
- **Choice**: `planWholeJob` emits `status=BASELINE_B0`; P2Status statistics
  exclude it.
- **Reason**: avoids counting every B0 run as a whole-job fallback.
- **Affected**: `Phase2Types.*`, `Phase2Engine.*`, `phase2/engine.py`,
  `scripts/phase2/v21_validate_e2e.py`.

## V21-08 — DEP_RELEASE negative overhead supersedes decision #007

- **Situation**: v2 allowed the host to read during PIM mode, producing −36%.
- **Choice**: model host readability as `max(t_release,t_exit)`; all release
  overheads are now >= 0. The v2 negative value is retained only in the P2-T2
  comparison table.
- **Reason**: PIM/host mode mutual exclusion is a hardware constraint.
- **Affected**: `phase2/timing.py`, P2-F3/P2-T1.

## V21-09 — L2 `correct=0` outside the main matrix are expected/known negatives

- **Situation**: the v2.1 L2 CSV has 501 `correct=0` rows.
- **Choice**: report them explicitly: 450 radius cases (uncovered distance
  classes) and 51 status cases (30 CHECKPOINT_MISSING + 21 DELTA_SHORT_BANK
  silent failures). Main/window/late are 100% correct.
- **Reason**: a raw incorrect count without context would look like a regression.
- **Affected**: `report/phase2_results_v21.md`, `results/phase2/v21/l2/matrix_summary.json`.
