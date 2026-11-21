# Phase 2 figure captions

## p2f1_selectivity

P2-F1. Report granularity x policy selectivity for UE reports, n_tiles=32. (a) replayed tile operations as a percentage of all job tile operations; (b) recovery latency normalized to B0 for the same workload, granularity, delay and seed. X labels use bk=bank, pg=page, cw=codeword. Bars are means with 95% bootstrap CI; black short lines are the Oracle lower bound. W3 panel uses K=4; K=16 rows are in statistics.csv. ASSUMED: L2 costs for MUL/RELU.

- Sample rows: 7560
- Source: `results/phase2/figure_data/` (L2 unless a P1-F or L3 source column is present).
- ASSUMED: MUL/RELU per-tile cost until L3 correction; silent distance distribution; single channel; host copy/OS overhead not modelled.

## p2f2_breakdown

P2-F2. Recovery-latency breakdown for W1, page reports, n_tiles in {8,32}. Bars are means over 30 seeds x fault operands: quiesce, restore, replay, resume and publish. Diamonds are L3 measured totals for the E2E cases that exist (B0 UE, B1 UE, B2 CE at n=8); missing diamonds mean no L3 case was run for that exact combination.

- Sample rows: 2160
- Source: `results/phase2/figure_data/` (L2 unless a P1-F or L3 source column is present).
- ASSUMED: MUL/RELU per-tile cost until L3 correction; silent distance distribution; single channel; host copy/OS overhead not modelled.

## p2f3_release

P2-F3. No-fault steady-state overhead versus release delay Delta/T_job for GLOBAL_HOLD and DEP_RELEASE, W1 and W2. Lines are L2 means over 30 seeds with bootstrap CI shading; black x markers are the Phase 1 P1-F measured points for W1 (HOST consumer). Negative DEP_RELEASE values mean the host starts reading a tile before the kernel-level producer barrier.

- Sample rows: 2280
- Source: `results/phase2/figure_data/` (L2 unless a P1-F or L3 source column is present).
- ASSUMED: MUL/RELU per-tile cost until L3 correction; silent distance distribution; single channel; host copy/OS overhead not modelled.

## p2f4_radius

P2-F4. Missed-error coverage versus replay cost. Each curve is a silent distance class (CE report plus one latent error); markers are B2, B2R-0, B2R-1, B2R-2 and B0. Coverage uses Wilson 95% CI over 30 seeds; the distance distribution is ASSUMED, not measured. A minus-class case whose primary tile lies on the first row falls back to the corresponding plus class, so some minus and plus curves overlap.

- Sample rows: 900
- Source: `results/phase2/figure_data/` (L2 unless a P1-F or L3 source column is present).
- ASSUMED: MUL/RELU per-tile cost until L3 correction; silent distance distribution; single channel; host copy/OS overhead not modelled.

## p2f5_status

P2-F5. P2Status distribution over 30 seeds for the status scenarios: normal UE, CE, Delta_cfg/Delta_true=0.5, Delta_cfg/Delta_true=1.0 with early consumption, missing checkpoint, truncated ledger and report-queue overflow.

- Sample rows: 210
- Source: `results/phase2/figure_data/` (L2 unless a P1-F or L3 source column is present).
- ASSUMED: MUL/RELU per-tile cost until L3 correction; silent distance distribution; single channel; host copy/OS overhead not modelled.

## p2f6_validation

P2-F6. L2-predicted versus L3-measured recovery cycles after refitting the W1 per-tile cost on the L3 E2E-0 no-fault runs; W2 is the held-out validation set. Hollow points are the W1 fit set, filled points are W2. The dashed line is y=x. Both MAPE values are printed in the panel; the W2 validation MAPE is the out-of-sample number.

- Sample rows: 120
- Source: `results/phase2/figure_data/` (L2 unless a P1-F or L3 source column is present).
- ASSUMED: MUL/RELU per-tile cost until L3 correction; silent distance distribution; single channel; host copy/OS overhead not modelled.

## p2t1_ledger

P2-T1. Ledger overhead per job: entries, estimated bytes and peak held data items under GLOBAL_HOLD and DEP_RELEASE. Values are produced by the L2 ledger model (`source=L2`).
