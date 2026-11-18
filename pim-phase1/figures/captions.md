# Figure captions and data provenance

Paper figures are rendered from `results/figure_data/*.csv` by
`scripts/plot_paper.py`; supplement figures are rendered from
`results/supplement/figure_data/*.csv` by `scripts/plot_supplement.py`.
Statistics use `STAT_SEED=20261115` and 10000 bootstrap resamples; proportions
use Wilson 95% intervals. Plot manifests are `figures/plot_manifest.json` and
`figures/plot_manifest_supplement.json`; they record input/output SHA256,
columns, filters, sample counts, aggregation, and output hashes.

## Fig. 1 — Path coverage

`figures/fig01_path_coverage.{pdf,svg,png}`

- Source: `results/figure_data/fig1_path_coverage.csv`.
- Matrix: 4 profiles × 4 paths; F1 and F2 panels; 3 banks per cell.
- Cell text: `C` corrected, `R` reported, `I` invalid output, `U` uncovered
  (no correction, no report, no format event). Color = corrected fraction.
- Sample: 4 × 4 × 3 = 48 bank-level rows per panel.
- Units/statistics: counts over 3 banks; no confidence interval is claimed for
  a 3-bank deterministic protocol check.
- Failure handling: no P1-A failures; all 144 cases are contract-pass.
- Model assumption: profiles are model behaviors, not measurements of a
  commercial HBM-PIM product. NATIVE is the upstream no-extension control.

## Fig. 2 — Report latency

`figures/fig02_report_latency.{pdf,svg,png}`

- Source: `results/figure_data/fig2_report_latency.csv` and
  `results/figure_data/fig2_report_events.csv`.
- (a) x = configured transport delay D, categorical positions 0, 1, 16, 256,
  4096 cycles; y = observed report latency (log scale). D=0 remains a separate
  category and is not replaced by 1. Curves are means over banks 0/3/15.
- (b) cumulative delivered reports versus delivery cycle for the 32-read burst
  at I=8; one line per bank.
- Sample: 60 single-event cases (2 granularities × 5 D × 2 I × 3 banks) plus
  3 burst cases × 32 reports.
- Failure handling: no dropped reports in these cases; queue depth is reported
  in the CSV.
- Model assumption: latency is the configured queue/transport model; matching
  D does not prove hardware latency.

## Fig. 3 — Control window

`figures/fig03_control_window.{pdf,svg,png}`

- Source: `results/figure_data/fig3_control_window.csv`.
- Four panels: n_tiles ∈ {1,8} × consumer ∈ {HOST,PIM(W2)}.
- x = H/T_ref, y = D/T_ref, color = timely-control fraction
  (`TIMELY_CONTROL` / all cases). Cell labels are percentages.
- Sample: 30 seeds per cell (1800 main cases total). No main case has
  `output_changed=true`; late/no-report cases are still not called timely
  control.
- Denominator: all 30 seeds, including late control and no-report outcomes;
  no-report cases are not silently dropped.
- Failure handling: zero main failures/timeouts. P1-C LOCAL_HIDDEN/HOST_ONLY
  controls are excluded from this main figure and summarized in the report.
- Model assumption: D/H are model parameters derived from T_ref; the figure
  does not claim hardware control latency.

## Fig. 4 — Quiescence

`figures/fig04_quiescence.{pdf,svg,png}`

- Source: `results/figure_data/fig4_quiescence.csv`.
- (a) x = feeder outstanding limit K; y = quiescence latency
  (`ACK − request`, cycles); lines = SYNTHETIC_BEFORE/QUEUE/INFLIGHT/NEAR;
  dotted black line = mean over actual-CE L values.
- (b) independent-channel progress: channel 0 data-access delta after ACK
  (0) versus channel 1 delta (56) in the two-channel case.
- Sample: 12 synthetic probes, 9 actual-CE cases, 1 two-channel case.
- Failure handling: all 22 cases ACKed; every `data_delta_after_ack=0`; no
  timeout. REF/clock continuation is recorded in the CSV and report.
- Model assumption: synthetic probes are direct control requests, not
  hardware pause measurements.

## Fig. 5 — Restore

`figures/fig05_restore.{pdf,svg,png}`

- Source: `results/figure_data/fig5_restore.csv`.
- x = n_tiles (1, 8); stacked bars = mean input-restore and W1 re-execution
  cycles; black marker/error bar = mean ACK→publish recovery with 95%
  bootstrap CI over 10 seeds. Individual seed values are the small points.
- Sample: 10 report-triggered positive cases per size (20 total).
- Text: 20/20 positive outputs bit-exact; 7/7 expected negatives rejected.
- Failure handling: all positive and expected-negative runs are present; no
  timeout or missing run is removed.
- Model assumption: checkpoint is a protected host-area model; restore writes
  use the simulated memory path, not real host/interconnect costs.

## Fig. 6 — Cost

`figures/fig06_cost.{pdf,svg,png}`

- Source: `results/figure_data/fig6_cost.csv`.
- (a) x = policy (H0, H0.25T, H1T); y = paired fault-free simulation-cycle
  overhead `T_variant/T_EXT_OFF − 1` in percent; bars grouped by
  n_tiles/consumer; error bars are 95% bootstrap CIs over 30 paired seeds.
- (b) resident space in KiB: checkpoint, held output, parity, runtime metadata
  for n_tiles 1 and 8.
- Sample: 480 cost cases (2 sizes × 2 consumers × 30 seeds × 4 policies).
- Failure handling: all 480 cases output-bit-exact; no missing or timeout.
- Model assumption: EXT_OFF is the same bounded feeder with extensions off.
  H0 has zero cycle overhead in this model; its cost is metadata and held
  output space. Wall-clock overhead is recorded separately in the CSV and is
  host-load dependent; it is not used as a hardware claim.

## Supplementary Fig. S1 — Robustness

`figures/figS1_robustness.{pdf,svg,png}`

- Source: `results/figure_data/fig6_robustness.csv`.
- x = service interval I; y = delivered reports / 32; panels drop=0 and
  drop=0.1. drop=0.1 injects deterministic observation gaps at detection
  sequence 10/20/30.
- Sample: 3 Q × 3 I × 2 drop rates = 18 cases.
- Failure handling: queue-drop and injected-drop accounting is exact; overflow
  is recorded, not hidden.
- Model assumption: drop is an observation-gap injection, not a hardware
  error model.

## Rendering and checks

- Matplotlib Agg, PDF font type 42, white background, 300 dpi PNG; DejaVu Sans
  8–10 pt; no gradients, 3-D bars, dual axes, or decorative backgrounds.
- All six required figures and the supplementary robustness figure were
  inspected at PNG size; grayscale copies are under `figures/gray/`.
- The caption/source table above is intended to travel with the figures and is
  also summarized in `report/phase1_results.md`.

## Supplement figures

The supplement figures are rendered by `scripts/plot_supplement.py` from
`results/supplement/figure_data/*.csv`. Their manifest is
`figures/plot_manifest_supplement.json` and records input/output SHA256,
columns, filters, sample counts, aggregation and model assumptions.

## Supplementary Fig. 7 — Escape efficacy

`figures/fig07_escape_efficacy.{pdf,svg,png}`

- Source: `results/supplement/figure_data/fig7_escape.csv`.
- Matrix: four constructed escape configurations: F3M (miscorrection with a
  CE report), F1W_F4S SAME_BANK, F1W_F4S DIFF_BANK, and F4S (silent, no report);
  n_tiles=8, HOST consumer. x = H/T_ref; y = D/T_ref.
- Color: CORRUPT_CONSUMED / (n − MASKED); n′ = 30 − MASKED. Cell text:
  `C` = corrupt-consumed count; `P` = prevented count. The panel subtitle gives
  n′ and MASKED. MASKED means FP16 rounding hides the corrupted value; it is
  excluded from the fraction denominator but reported separately.
- Sample: 4 configurations × 9 D/H cells × 30 seeds = 1080 cases.
- Failure handling: all 1080 cases are contract-pass; no case is all-masked in
  this matrix. Failures/timeouts are listed in the supplement validation and
  are not plotted as zero.
- Model assumption: F3M/F4S are constructed CoF-style bit patterns, not a
  Rowhammer probability model. F3M and F4S deliver the same bad data; only
  F3M produces a report. This panel uses the LOCAL_VISIBLE bank report model
  and the HOST consumer only; the full size/consumer matrix is Fig. S2.

## Supplementary Fig. 8 — Report-sequence ambiguity

`figures/fig08_report_ambiguity.{pdf,svg,png}`

- Source: `results/supplement/figure_data/fig8_pairs.csv` (from the 330-case
  `fig8_signatures.csv`).
- Layout: rows scrub=0/1; columns bank/page/codeword. Each panel is a
  scenario × scenario matrix. Scenarios: S1 one error bit; S2 independent
  errors in different codewords (same burst); S3 different codeword, same bank
  and different row; S4 two susceptible bits in one codeword (danger case);
  S5 different bank; S6 the same bit flipped again after scrub.
- Color: fraction of 10 seeds whose public `report_signature` is identical.
  Off-diagonal cells only; the diagonal is omitted. A blank cell means 0/10;
  cells with k>0 are annotated `k/10`. Hatching marks S6 at scrub=0, which was
  not run because a second XOR at the same bit would undo the first.
- Sample: 330 cases = 3 granularities × (5 scenarios × scrub 0/1 + S6 scrub 1)
  × 10 seeds; 90 scenario-pair rows.
- Failure handling: 330/330 contract-pass; 0 missing/timeout. S6 scrub=0 is
  explicitly N/A, not 0%.
- Model assumption: bank/page are model report granularities; codeword is an
  ASSUMED interface. scrub is an idealized zero-cycle, zero-transaction
  rewrite. `report_signature` excludes `report_cycle`; the timing-inclusive
  signature is only in the CSV and is not the main figure.

## Supplementary Fig. S2 — Escape by configuration

`figures/figS2_escape_by_config.{pdf,svg,png}`

- Source: `results/supplement/figure_data/fig7_escape.csv`.
- Matrix: 4 rows = n_tiles 1/8 × consumer HOST/PIM; 4 columns = the same F3M,
  F1W SAME_BANK, F1W DIFF_BANK and F4S configurations as Fig. 7.
- Color and cell text are defined as in Fig. 7: color = CORRUPT_CONSUMED /
  (n − MASKED); `C` and `P` are counts; panel subtitles give n′ and MASKED.
- Sample: 4 × 4 panels × 9 D/H cells × 30 seeds = 4320 cases.
- Failure handling: all 4320 cases are contract-pass; no case is all-masked.
  MASKED is excluded only from the fraction denominator and is never merged
  into PREVENTED.
- Model assumption: all configurations are constructed escape patterns under
  LOCAL_VISIBLE bank reports; n′ and MASKED are data-dependent, so the figure
  must not be read as a hardware error-rate map.
