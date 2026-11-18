# HBM-PIM Phase 1: ECC Reporting, Output Control, Pause and Recovery

This report summarizes the Phase 1 simulation study implemented in this
workspace. It is a model study, not a measurement of a commercial HBM-PIM
product. All numbers below are traceable to the saved run tree, the
figure-data CSVs, and `results/statistics.csv`.

## 1. Research question and scope

The study asks whether a PIM kernel's operand reads and result writes can be
covered by an explicit ECC model; whether the runtime can observe the resulting
errors, their coarse location and their notification time; whether a host or
device consumer can be prevented from consuming an untrusted output; whether a
target channel can be quiesced without stopping refresh; and whether a
trusted-input checkpoint plus PIM state reset can restart the whole kernel.

The Phase 1 scope is deliberately narrow:

- one HBM-PIM device model with 16 banks, FP16, Scheme8 addressing and the
  upstream timing parameters;
- one producer kernel W1 = A + X, with optional W2 = B × 2 for a PIM consumer;
- host/PIM paths, four ECC profiles, coarse bank/page reports, output version
  gating, channel quiescence, whole-kernel recovery, and no-fault cost;
- no claim of real Rowhammer probability, real ECC layout, or a deployed
  operating-system notification path.

The supplement extends this scope with two additional questions:

- supplement P1-G: constructed ECC escapes (miscorrection F3M, silent F4S,
  CE witness + silent) to test whether timely control withholds an actually
  corrupted output;
- supplement P1-H: multi-report sequences under bank/page/codeword granularity
  with and without scrub, to test what coarse reports can distinguish.

The supplement uses an independent 4650-case manifest. It is not counted in the
2602 paper-matrix cases and does not change any paper result.

## 2. Fixed platform and assumptions

| item | value |
|---|---|
| upstream | `SAITPublic/PIMSimulator` |
| commit | `3703d1f19c8f027360cc33a3243eb271e3bb6898` |
| model | HBM2 2M x 16B x64, 16 banks, 1 rank, 1 logical channel (2 for isolation) |
| data storage | `NO_STORAGE=0` |
| workload | W1 FP16 ADD, `B=A+X`; W2 `C=B×2` for PIM consumer |
| inputs | `A[i]=((i+seed) mod 17-8)/8`, `X[i]=((3i+seed) mod 13-6)/8` |
| ECC | research SECDED(72,64), four codewords per 32 B burst |
| report fields | `report_seq, report_cycle, error_class, region_type, region_id` |
| main profile | LOCAL_VISIBLE (ASSUMED) |

The four profiles are model behaviors:

- `NATIVE`: no new ECC/report/control behavior; upstream control.
- `LOCAL_VISIBLE`: host and PIM storage data protected; CE/UE reportable.
- `LOCAL_HIDDEN`: same data protection, but CE is not delivered to runtime.
- `HOST_ONLY`: host paths protected; PIM read/write gap, result format may be
  invalid.

`LOCAL_VISIBLE`, `LOCAL_HIDDEN` and `HOST_ONLY` are not claims that a real
Aquabolt-XL exposes these interfaces. They are explicit model assumptions.

## 3. Method and correctness

### 3.1 Bit-exact reference

Every W1 case uses the same input generator in C++ and in an independent
Python/NumPy reference. The final B output is compared as raw FP16 bytes, not
with a numerical tolerance. W2 compares `(A+X)×2` in the same FP16 order. The
final paper matrix has no failed output comparison: 2595 successful cases plus
7 expected negatives, out of 2602 manifest cases.

The G8 manifest covers:

| group | cases | scope |
|---|---:|---|
| P1-A | 144 | 4 profiles × 4 paths × F0/F1/F2 × bank 0/3/15 |
| P1-B | 63 | bank/page, D∈{0,1,16,256,4096}, I∈{1,8}, 3 banks, plus burst |
| P1-C | 1848 | 1800 main + 48 LOCAL_HIDDEN/HOST_ONLY controls |
| P1-D | 22 | 12 synthetic + 9 actual CE + 1 two-channel |
| P1-E | 27 | 20 positive + 5 status negatives + 2 initialization controls |
| P1-F cost | 480 | 2 sizes × 2 consumers × 30 seeds × 4 policies |
| P1-F robustness | 18 | 3 Q × 3 I × 2 drop rates |

The supplement uses a separate manifest and is not part of the 2602 cases:

| supplement group | cases | scope |
|---|---:|---|
| P1-G Escape | 4320 | n_tiles 1/8 × HOST/PIM × 30 seeds × 4 fault configurations × 3 D/T_ref × 3 H/T_ref |
| P1-H Sequence | 330 | 10 seeds × 3 granularities × (S1–S5 with/without scrub + S6 with scrub) |
| **total** | **4650** | 4650 SUCCESS, 0 FAIL, 0 TIMEOUT, 0 MISSING |

### 3.2 Truth isolation

The runtime sees only public report fields. `truth.csv` is written by the
evaluator and is not read by the controller; the recovery source is the
trusted A/X checkpoint, not `reference.bin`. The independent reference is used
only after the run to compare saved output bytes.

### 3.3 Statistical conventions

Continuous metrics use seed-level means; the main intervals are 95% bootstrap
intervals over 10,000 resamples with fixed statistic seed `20261115`. Binary
proportions use Wilson 95% intervals. A 100% result on 30 seeds has a Wilson
lower bound of 88.6%, so it is not presented as proof of zero failures in the
population. No p99 is reported for 30 scenarios.

## 4. Results

### 4.1 P1-A: protection path and data correctness

Fig. 1 is the path-coverage matrix. For F1 single-bit errors:

| profile | HREAD | PREAD | PWRITE_HREAD | PWRITE_PREAD |
|---|---:|---:|---:|---:|
| NATIVE | 0/3 correct | 0/3 | 0/3 | 0/3 |
| LOCAL_VISIBLE | 3/3 corrected, 3 reported | 3/3 corrected, 3 reported | 3/3 corrected, 3 reported | 3/3 corrected, 3 reported |
| LOCAL_HIDDEN | 3/3 corrected, 0 reported | 3/3 corrected, 0 reported | 3/3 corrected, 0 reported | 3/3 corrected, 0 reported |
| HOST_ONLY | 3/3 corrected, 3 reported | 0/3 | 0/3 | 0/3 |

For F2 same-codeword double-bit errors, LOCAL_VISIBLE/LOCAL_HIDDEN produce UE
and do not recover the output; HOST_ONLY exposes the PIM path gap; NATIVE
produces no report and no correction. Report count and correction are separate
capabilities: LOCAL_HIDDEN corrects data without delivering CE, while
HOST_ONLY delivers reports only on paths that actually use host ECC.

Evidence: `results/figure_data/fig1_path_coverage.csv`,
`results/statistics.csv`, `figures/fig01_path_coverage.*`.

### 4.2 P1-B: report location and notification latency

For a single W0/HREAD event, observed latency follows the configured transport
delay: D=0/1 gives 1 cycle, D=16/256/4096 gives 16/256/4096 cycles. The
32-read burst at I=8 delivers all 32 CE reports without drops; the report
sequence is FIFO, service gaps are at least 8 cycles, and bank/page region
identity is checked against the actual access tuple. The queue is not the
bottleneck in these single-event cases.

Evidence: `results/figure_data/fig2_report_latency.csv`,
`fig2_report_events.csv`, `figures/fig02_report_latency.*`.

### 4.3 P1-C: output visibility and control window

The main matrix is 1800 LOCAL_VISIBLE cases: 30 seeds × 2 n_tiles × 2
consumers × 5 D/T_ref × 3 H/T_ref. Overall:

```text
TIMELY_CONTROL   1118 / 1800  (62.1%, Wilson 95% CI 59.8–64.3%)
LATE_CONTROL      682 / 1800  (37.9%, Wilson 95% CI 35.7–40.2%)
output_changed      0 / 1800
```

The zero `output_changed` count is a consequence of the model: a single-bit CE
is corrected before the PIM arithmetic, so a late report does not imply a wrong
numerical B in this experiment. It does mean the output version was not
controlled before consumption; these outcomes are not called timely control.

Selected cells (timely fraction over 30 seeds):

| n_tiles/consumer | D/T_ref, H/T_ref | timely |
|---|---|---:|
| 1 HOST/PIM | 0, any H | 100% |
| 1 HOST/PIM | 0.5, H=0 | 43.3% (Wilson 27.4–60.8%) |
| 1 HOST/PIM | 1, H=1 | 100% |
| 1 HOST/PIM | 2, any H | 0% |
| 8 HOST | 0.5, H=0 | 36.7% |
| 8 HOST | 0.5, H=0.25T | 76.7% |
| 8 HOST | 1, H=0.25T | 6.7% |
| 8 HOST | 1, H=1 | 100% |
| 8 PIM | 1, H=0.25T | 20.0% |

The 48 extra controls are not part of the main fraction:

- LOCAL_HIDDEN: 24 cases, CE not delivered, 24 `NO_REPORT`, output remains
  bit-exact.
- HOST_ONLY: 24 cases, PIM format gap, 24 `FORMAT_UNSUPPORTED`; these are not
  counted as recovery success.

Fig. 3 shows the full D/H heatmaps for HOST and PIM consumers.
Evidence: `results/figure_data/fig3_control_window.csv`,
`results/statistics.csv`.

### 4.4 P1-D: quiescence and independent channels

All 22 cases ACKed with no timeout, and every case has
`data_delta_after_ack=0`:

| trigger | K=1 | K=8 | K=32 |
|---|---:|---:|---:|
| SYNTHETIC_BEFORE | 1 | 1 | 1 |
| SYNTHETIC_QUEUE | 21 | 21 | 21 |
| SYNTHETIC_INFLIGHT | 10 | 32 | 66 |
| SYNTHETIC_NEAR | 43 | 65 | 402 |
| ACTUAL_CE (mean over L) | ~13 | ~48 | ~48 |

The near-completion and high-K cases show the real drain bottleneck. The
two-channel case stops channel 0 (data delta 0 after ACK) while channel 1
advances by 56 data accesses; this is not a claim of zero cross-channel
performance impact. The 8000-cycle observation window advances the clock and
refresh counters; no REF was stopped.

Evidence: `results/figure_data/fig4_quiescence.csv`,
`figures/fig04_quiescence.*`.

### 4.5 P1-E: trusted-input recovery

All 20 report-triggered positive cases (`n_tiles∈{1,8}`, seeds 0..9) publish a
new version and are bit-exact. All 7 expected negatives are rejected with the
required status or caught by the initialization controls.

| n_tiles | checkpoint | restore writes | restore cycles | reexec cycles | ACK→publish |
|---:|---:|---:|---:|---:|---:|
| 1 | 8192 B | 256 | 557 | 804 | 1361 |
| 8 | 65536 B | 2048 | 4274 | 4130 | 8404 |

The old output is not published; the old handle becomes stale; the new version
is `PUBLISHED`. The recovery values are deterministic in this model, so the
bootstrap interval has zero width. The negative controls show that skipping
the input restore leaves a raw A error and 2 CE reports, while skipping state
reset leaves `HAB_PIM` and blocks restore writes until the watchdog.

Evidence: `results/figure_data/fig5_restore.csv`,
`figures/fig05_restore.*`, `docs/g7_recovery.md`.

### 4.6 P1-F: fault-free cost and robustness

The no-fault cost is paired per seed against EXT_OFF:

| policy | mean simulation-cycle overhead | min | max |
|---|---:|---:|---:|
| LOCAL_VISIBLE_H0 | 0.00% | 0.00% | 0.00% |
| LOCAL_VISIBLE_H0.25T | 10.21% | 7.66% | 13.25% |
| LOCAL_VISIBLE_H1T | 43.96% | 30.70% | 53.00% |

These are cycle-model costs, not hardware wall-clock. H0 has no simulated-cycle
overhead; its cost is 8/64 KiB checkpoint, 4/32 KiB held output, 1.5/16 KiB
parity and 64 B metadata. Wall-clock overhead is saved separately and is
host-load dependent.

Robustness: Q=64 delivers all 32 burst reports even at I=64; Q=1 at I=64
delivers 3/32 with 29 queue drops. With drop=0.1, exactly 3 deterministic
observation gaps are injected at detection sequence 10/20/30, giving 29/32
delivered. Overflow and drop accounting are exact.

Evidence: `results/figure_data/fig6_cost.csv`,
`fig6_robustness.csv`, `figures/fig06_cost.*`, `figures/figS1_robustness.*`.

### 4.7 P1-G (supplement): does timely control stop a corrupted value?

P1-G constructs ECC escapes rather than relying on a corrected single-bit CE.
F3M flips data bits {0,1,4}: the decoder returns CORRECTED, reports a CE, but
"corrects" data bit 10 and returns a wrong value. F4S flips {0,1,4,10}: the
syndrome is zero, the decoder returns CLEAN, and there is no report. Both modes
deliver exactly the same bad 64-bit value. F1W_F4S adds a corrected CE witness
in one burst and a silent F4S in another burst, either in the same bank or a
different bank. The evaluator runs only after the case, reads the held B storage
for the corrupt burst, and never feeds truth or the corrupt address to the
runtime. `PREVENTED` requires the output to be unpublished, the runtime lock to
have fired, and the held B to be actually different from the fault-free B;
`MASKED` means FP16 rounding made the final B/C byte-identical. MASKED is
excluded from the control-fraction denominator and reported separately.

For the n_tiles=8 HOST panel of Fig. 7, the 270 cases per fault configuration
give:

| fault configuration | PREVENTED | CORRUPT_CONSUMED | MASKED | total |
|---|---:|---:|---:|---:|
| F3M | 172 | 80 | 18 | 270 |
| F4S | 0 | 252 | 18 | 270 |
| F1W_F4S SAME_BANK | 180 | 81 | 9 | 270 |
| F1W_F4S DIFF_BANK | 180 | 81 | 9 | 270 |

Across the full 4320-case matrix the totals are PREVENTED 2166,
CORRUPT_CONSUMED 1920, and MASKED 234. The shape is the expected control-window
pattern: F3M prevents at small D and/or large H but consumes when the report is
late; every unmasked F4S case is consumed because there is no report.

For F3M, 28 of the 36 Fig. 3 cells have exactly the same fraction as
TIMELY_CONTROL; the other 8 differ by at most 5.7 percentage points (3/9 in the
n_tiles=8 HOST panel, all at D/T_ref 0.5 or 1). The difference is methodological:
Fig. 3 uses all 30 seeds and classifies TIMELY_CONTROL from the runtime, whereas
P1-G removes MASKED seeds and additionally requires the held B to be corrupt.
The grid pattern is consistent; the small differences are not tuned away.
The supplement reports 8/36 rather than forcing the pre-registered 27-cell
denominator, because the executed matrix has 36 F3M cells. F1W_F4S SAME_BANK
and DIFF_BANK have identical outcomes in 990/1080 matched cases; the 90
differences are all one side MASKED while the other propagates, never a case
where both propagate and one is prevented while the other is consumed. DIFF_BANK
has `report_covers_corrupt_bank=false` in 1080/1080 cases, while SAME_BANK has
true in 1080/1080: the current runtime locks the whole kernel on the witness CE,
so it does not rely on the report covering the corrupt bank. A future
region-selective recovery would miss the DIFF_BANK damage.

Evidence: `results/supplement/figure_data/fig7_escape.csv`,
`results/supplement/statistics_supplement.csv`,
`figures/fig07_escape_efficacy.*`, `figures/figS2_escape_by_config.*`.

### 4.8 P1-H (supplement): what a sequence of coarse reports can and cannot distinguish

P1-H has 330 cases: 10 seeds over S1–S6, three public report granularities
(bank, page, and the ASSUMED codeword interface), with and without the ASSUMED
idealized scrub. S1 is one persistent bit; S2 is two independent errors in different
codewords of one burst; S3 is the same bank and a different row; S4 is two
susceptible bits in one codeword (the dangerous case); S5 is a different bank;
S6 is the same bit flipped again after scrub. Each round reads P, Q, R in the
same order. The public signature is `error_class:region_id` ordered by
`report_seq` and excludes `report_cycle`.

Only non-singleton equivalence groups are listed (10/10 seeds with identical
signatures):

| scrub | bank | page | codeword |
|---|---|---|---|
| 0 | S2 ≡ S3 | S2 ≡ S5 | none |
| 1 | S2 ≡ S3 ≡ S4 ≡ S6 | S2 ≡ S4 ≡ S5 ≡ S6 | S4 ≡ S6 |

Thus bank granularity cannot separate the dangerous S4 from the benign S2, S3
or S6; page granularity also groups S4 with S2/S5/S6; codeword granularity
separates S2 and S3 from S4, but still cannot separate S4 from S6. This is the
central P1-H result: finer reports help, but do not reveal whether two
susceptible bits share one codeword.

The CoF-style rule (fire when one region receives at least two CEs) gives:

| granularity, scrub=1 | scenarios firing 10/10 |
|---|---|
| bank | S2, S3, S4, S6 |
| page | S2, S4, S5, S6 |
| codeword | S4, S6 |

The rule fires on the dangerous S4, but also on benign S2/S3/S6 (bank) or S6
(codeword); it therefore cannot identify S4 by itself. With scrub=0, S4
produces UE in 10/10 seeds at all three granularities, so it becomes
distinguishable only after the error is already uncorrectable. No other
scenario produces UE.

Evidence: `results/supplement/figure_data/fig8_signatures.csv`,
`fig8_pairs.csv`, `fig8_cof_rule.csv`, `figures/fig08_report_ambiguity.*`.

## 5. Failure categories and expected negatives

- Manifest: 2602 cases.
- Success: 2595.
- Expected negative: 7.
- Failure: 0.
- Timeout: 0.
- Missing: 0.

The 7 expected negatives are `CHECKPOINT_MISSING`, `VERSION_MISMATCH`,
`CHECKPOINT_CORRUPT`, `ALREADY_COMMITTED`, `STALE_HANDLE`,
`NO_INPUT_RESTORE` and `NO_STATE_RESET`. `PIMBenchFixture.add` remains the
known upstream performance failure (1.98597 < 2.0); it is not counted as a
Phase 1 pass and no threshold was changed.

Supplement: the independent 4650-case manifest has 4650 SUCCESS, 0 FAIL, 0
TIMEOUT, 0 MISSING and 0 expected negatives. It is reconciled separately from
the paper matrix and is not added to the 2602 count.

## 6. Figures and data

| figure | data | sample |
|---|---|---:|
| Fig. 1 path coverage | `fig1_path_coverage.csv` | 144 P1-A |
| Fig. 2 report latency | `fig2_report_latency.csv`, `fig2_report_events.csv` | 63 P1-B |
| Fig. 3 control window | `fig3_control_window.csv` | 1800 main |
| Fig. 4 quiescence | `fig4_quiescence.csv` | 22 P1-D |
| Fig. 5 restore | `fig5_restore.csv` | 20 P1-E |
| Fig. 6 cost | `fig6_cost.csv` | 480 P1-F cost |
| Fig. S1 robustness | `fig6_robustness.csv` | 18 P1-F robust |
| Fig. 7 escape efficacy | `results/supplement/figure_data/fig7_escape.csv` | 1080 (n_tiles=8 HOST) |
| Fig. 8 report ambiguity | `results/supplement/figure_data/fig8_pairs.csv` | 330 cases / 90 pairs |
| Fig. S2 escape by config | `results/supplement/figure_data/fig7_escape.csv` | 4320 |

`figures/captions.md` defines every cell, unit, denominator, filtering rule and
model assumption. `figures/plot_manifest.json` records input/output hashes,
columns, filters, sample counts and aggregation. `results/statistics.csv`
contains 242 statistics rows with numerator/denominator, n, mean/median/min/max
and Wilson/bootstrap intervals where defined.

## 7. Platform capability judgment

Within the model, Phase 1 supports a Phase 2 study under these conditions:

- LOCAL_VISIBLE with bank/page reports, a bounded report queue, and a runtime
  that can hold an output version and quiesce the target channel;
- a trusted A/X checkpoint and a whole-kernel restart, not an in-place repair
  of a committed output;
- single-channel or independent-channel traffic, with a channel-level pause;
- supplement: timely control withholds an actually corrupted constructed
  escape output when a report arrives in the same kernel (P1-G); F4S without a
  report is consumed.

It does **not** establish:

- that a commercial HBM-PIM product exposes these ECC/control interfaces;
- that LOCAL_HIDDEN reports are available to an OS/runtime;
- that HOST_ONLY PIM paths are recoverable (they can be format-unsupported);
- that coarse (bank/page/codeword) reports identify whether multiple
  susceptible bits share one codeword; P1-H shows that bank/page cannot
  separate S4 from benign scenarios, and codeword still cannot separate S4
  from S6;
- that a persistent or ongoing Rowhammer attack is isolated; the study injects
  one storage fault and does not continue injection during recovery.

## 8. Limitations and unmodeled factors

- ECC layout is a research SECDED model, not a real HBM codeword layout.
- ECC pipeline/energy costs are not modeled; the 0.0% H0 cycle result must not
  be read as a hardware cost claim.
- Host copy, interconnect, OS notification and CPU handling costs are excluded.
- Report latency D, handler L, hold H and pause scope are model parameters.
- P1-C uses the direct memory path (same as the G5 smoke), so its T_ref is a
  direct-path value; P1-F cost uses the bounded feeder.
- The two-channel case uses independent host traffic, not a full W1/W3 kernel
  pair.
- P1-F drop is a deterministic observation-gap injection, not a hardware error
  model.
- The study uses 30 scenarios per main config; it does not estimate a
  population failure probability.
- Supplement: scrub is an idealized zero-cycle, zero-transaction rewrite; the
  codeword report granularity is an ASSUMED interface; F3M/F4S are constructed
  CoF-style patterns, not a Rowhammer probability model.
- Supplement injections are performed once before the PIM kernel starts; the
  runtime locks the whole kernel output, not only a reported region, and P1-H
  uses 10 seeds per (scenario, granularity, scrub).

## 9. Reproducibility

The paper matrix is reconstructible from:

```bash
.venv/bin/python scripts/calibrate_g8.py
.venv/bin/python scripts/expand_suite.py --suite configs/suite_paper.yaml
.venv/bin/python scripts/run_suite.py --manifest configs/generated/paper-manifest.json --jobs 8
.venv/bin/python scripts/aggregate.py --runs runs/paper --out results
.venv/bin/python scripts/plot_paper.py --results results --out figures
```

The run tree includes `configs/generated/paper/<run_id>/config.json`,
`runs/paper/<run_id>/`, `environment/source.patch`, `environment/source_g8.patch`,
G0-G8 locks, and the clean-replay smoke. The artifact and hash manifest are
described in `artifact/MANIFEST.sha256` and `artifact/ARCHIVE.sha256`.

The supplement is reconstructible from:

```bash
.venv/bin/python scripts/expand_supplement.py --suite configs/suite_supplement.yaml
.venv/bin/python scripts/run_suite.py \
  --manifest configs/generated/supplement-manifest.json \
  --results-dir results/supplement --runs-dir runs/supplement --jobs 8 --resume
.venv/bin/python scripts/aggregate_supplement.py
.venv/bin/python scripts/plot_supplement.py
```

The supplement manifest is `configs/generated/supplement-manifest.json`, the
run tree is `runs/supplement/<run_id>/`, and its evidence is under
`results/supplement/`. The smoke subset is
`configs/generated/supplement-smoke-manifest.json` (24 P1-G + 33 P1-H).

## 10. References

1. PIMSimulator, fixed commit `3703d1f...`.
2. Kim et al., “Flipping Bits in Memory Without Accessing Them,” ISCA 2014.
3. Aquabolt-XL, Hot Chips 33, 2021.
4. Copy-on-Flip, NDSS 2023.
5. Read Disturbance in HBM2, DSN 2024.
6. This project's `HANDOFF.md` and gate evidence.
