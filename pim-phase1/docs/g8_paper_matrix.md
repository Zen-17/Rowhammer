# G8 Paper Matrix Expansion and Execution

G8 expands the five experiment groups plus the P1-F cost/robustness supplement
into a hash-addressed manifest, executes every case, and reconciles the
manifest against success, expected-negative, failure, timeout, and missing
categories.

## 1. Manifest and reconciliation

```text
configs/suite_paper.yaml
configs/generated/paper-manifest.json
configs/generated/paper-index.csv
configs/generated/paper/<run_id>/config.json
runs/paper/<run_id>/
results/g8/validation.json
results/validation_summary.json
```

The expanded manifest contains **2602 cases**:

| group | cases | composition |
|---|---:|---|
| P1-A | 144 | 4 profiles × 4 paths × 3 faults × 3 banks |
| P1-B | 63 | 60 D/I/granularity/bank cases + 3 burst cases |
| P1-C | 1848 | 1800 main + 48 negative controls |
| P1-D | 22 | 12 synthetic + 9 actual CE + 1 two-channel |
| P1-E | 27 | 20 positive + 5 status negatives + 2 state/input controls |
| P1-F Cost | 480 | 2 n_tiles × 2 consumers × 30 seeds × 4 policies |
| P1-F Robustness | 18 | 3 Q × 3 I × 2 drop rates |
| **total** | **2602** | |

Execution result:

```text
SUCCESS             2595
EXPECTED_NEGATIVE      7
FAIL                   0
TIMEOUT                0
MISSING                0
```

`results/g8/validation.json` and `results/validation_summary.json` both pass the
reconciliation. The failure table is header-only, not deleted.

## 2. Calibration

`scripts/calibrate_g8.py` records T_ref from no-fault producer runs. T_ref
excludes input preload and includes mode preparation plus result writeback.

| path | n_tiles=1 | n_tiles=8 |
|---|---:|---:|
| P1-C direct producer | 731 | 3339 |
| P1-F feeder producer | 804 | 4130 |

P1-C uses `D=ceil(D_ratio*T_ref)` and `H=ceil(H_ratio*T_ref)`. P1-F uses the
feeder T_ref for H=0, H=0.25T, and H=1T. Calibration evidence is stored under
`runs/g8/calibration/` and `results/g8/calibration.json`.

P1-C target selection follows the protocol: `seed mod 3` selects the first,
middle, or last third of the A burst sequence, and `numpy.random.Generator(
PCG64(seed))` selects the exact burst inside that segment. The C++ fixture
does not resample; it reads `target_burst` from the manifest.

## 3. Group results

### P1-A and P1-B

- P1-A 144/144 contract-pass. The matrix covers NATIVE, LOCAL_VISIBLE,
  LOCAL_HIDDEN, HOST_ONLY; HREAD, PREAD, PWRITE_HREAD, PWRITE_PREAD; F0/F1/F2;
  bank 0/3/15.
- P1-B 63/63 contract-pass: bank/page granularity, D∈{0,1,16,256,4096},
  I∈{1,8}, bank 0/3/15; plus 32-read burst cases at I=8.
- Report latency and queue depth are in `results/figure_data/fig2_report_latency.csv`.

### P1-C main and controls

P1-C main: 30 seeds × 2 n_tiles × 2 consumers × 5 D/T_ref × 3 H/T_ref =
1800 cases, LOCAL_VISIBLE, bank reports, W1 F1.

Outcome distribution:

| n_tiles | consumer | TIMELY_CONTROL | LATE_CONTROL | NO_REPORT | FORMAT_UNSUPPORTED |
|---:|---|---:|---:|---:|---:|
| 1 | HOST | 283 | 167 | 12 | 12 |
| 1 | PIM | 283 | 167 | 12 | 12 |
| 8 | HOST | 274 | 176 | 0 | 0 |
| 8 | PIM | 278 | 172 | 0 | 0 |

The NO_REPORT/FORMAT_UNSUPPORTED entries in the n_tiles=1 rows are the 48
negative controls, not main LOCAL_VISIBLE samples:

- LOCAL_HIDDEN: 24 cases, CE not delivered => `NO_REPORT`; output remains
  bit-exact.
- HOST_ONLY: 24 cases, PIM path format gap => `FORMAT_UNSUPPORTED`; output
  validity is recorded separately and not counted as recovery success.

All 1848 P1-C cases have `contract_pass=true`; changed/invalid outputs are
reported through `output_changed`, `invalid_outputs`, and `outcome_class`.

### P1-D

22 cases:

- 12 synthetic control probes: before/queue/inflight/near × K∈{1,8,32};
- 9 actual CE cases: K∈{1,8,32} × L∈{0,8,64};
- 1 two-channel case.

`data_delta_after_ack=0` in every case; quiesce latency ranges from 1 to 402
cycles. The synthetic probes are labeled `SYNTHETIC_CONTROL`; L applies to the
report-triggered actual-CE cases, not to direct synthetic requests.

### P1-E

20 report-triggered positive cases (`n_tiles∈{1,8}`, seeds=0..9) all publish
a new version and are bit-exact. Recovery latency:

| n_tiles | restore cycles | reexec cycles | ACK->publish |
|---:|---:|---:|---:|
| 1 | 557 | 804 | 1361 |
| 8 | 4274 | 4130 | 8404 |

Seven expected negatives pass: five required statuses plus
`NO_INPUT_RESTORE` and `NO_STATE_RESET`.

### P1-F cost

480 no-fault cases. Paired overhead is computed per seed against EXT_OFF:

| policy | samples | mean overhead | min | max |
|---|---:|---:|---:|---:|
| LOCAL_VISIBLE_H0 | 120 | 0.0% | 0.0% | 0.0% |
| LOCAL_VISIBLE_H0.25T | 120 | 10.21% | 7.66% | 13.25% |
| LOCAL_VISIBLE_H1T | 120 | 43.96% | 30.70% | 53.00% |

H=0 adds no simulated cycles in this model; its cost is metadata and held
output space, not a measured latency. H=0.25T/H1T include the configured hold
window. `fig6_cost.csv` also records per-run `wall_seconds`, paired EXT_OFF
wall time, and a wall-overhead fraction; wall time is host-load dependent and
is reported separately from the simulated-cycle result. These are model
costs, not hardware measurements.

### P1-F robustness

18 cases: Q∈{1,8,64}, I∈{1,8,64}, drop∈{0,0.1}. 8 cases overflow; maximum
queue depth is 30. Drop=0.1 injects exactly 3 observation gaps per case, at
detection sequence 10, 20, 30. The queue/drop accounting is exact:
`forwarded + injected = total detections` and
`delivered + queue_drops = forwarded`.

## 4. Commands

```bash
.venv/bin/python scripts/calibrate_g8.py
.venv/bin/python scripts/expand_suite.py --suite configs/suite_paper.yaml
.venv/bin/python scripts/run_suite.py \
  --manifest configs/generated/paper-manifest.json --jobs 8
.venv/bin/python scripts/aggregate.py --runs runs/paper --out results
```

`run_suite.py` starts one simulator process per case, writes
`params.ini/config.json/metadata.json/stdout.log/stderr.log/exit_code`, and
per-case `validation.json`. `--resume` skips only runs whose binary, patch,
and config hashes match and whose validation passed.

## 5. Evidence

```text
configs/generated/paper-manifest.json
configs/generated/paper-index.csv
runs/paper/<run_id>/
results/g8/validation.json
results/g8/run_metrics.csv
results/g8/summary.csv
results/g8/failure_cases.csv
results/validation_summary.json
results/run_metrics.csv
results/summary.csv
results/figure_data/fig1_path_coverage.csv
results/figure_data/fig2_report_latency.csv
results/figure_data/fig3_control_window.csv
results/figure_data/fig4_quiescence.csv
results/figure_data/fig5_restore.csv
results/figure_data/fig6_cost.csv
results/figure_data/fig6_robustness.csv
logs/g8_calibration.log
logs/g8_expand.log
logs/g8_run_full.log
logs/g8_aggregate.log
```

## 6. Model boundaries

- P1-A/P1-B use the G4 all-path fixture with fixed seed=0 and bank-level
  physical checks.
- P1-C uses the direct memory path (same path as the G5 smoke) rather than the
  bounded feeder; its T_ref is therefore the direct-path value. This is a
  model deviation from the wording "same feeder" for P1-C and is recorded
  explicitly.
- P1-C PIM consumer executes W2 = B×2 on the device after the W1 output is
  published; the consume boundary is the first B read.
- P1-D synthetic probes are direct control requests; L is exercised only by
  the actual-CE cases.
- P1-E checkpoint is a model-side protected host area; restore writes traverse
  the simulated memory system but not real host/interconnect costs.
- P1-F cost uses the bounded feeder and EXT_OFF is the same feeder with
  extension off. The overhead is simulated-cycle cost, not wall-clock
  hardware cost.
- P1-F robustness drop policy is a deterministic observation-gap injection,
  not a hardware error model.
- No result here is a claim about a commercial HBM-PIM product or an ongoing
  Rowhammer attack.
