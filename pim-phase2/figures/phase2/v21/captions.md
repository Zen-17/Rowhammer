# Phase 2 v2.1 figure captions

## p2f1_selectivity

P2-F1. Report granularity x policy for UE reports, n_tiles=32. (a-c) affected tile operations as a fraction of all job tile operations; (d-f) fault_overhead_cycles normalized to B0 for the same key. Columns are transport delays D={16,256,4096}. B1/B2 are drawn; B0 is the whole-job baseline (100% replay) and is the 1.0 line in the lower row. Bank reports use the v2.1 R_read filter and replay coalescing. Oracle is the lower bound. ASSUMED: L2 MUL/RELU costs from the v2.1 L3 no-fault fit.

- Sample rows: 4320
- Source: `results/phase2/v21/figure_data/`.
- ASSUMED: W4/MUL/RELU cost fits are L3-derived but not hardware-measured; MIX-A/MIX-B are illustrative; single channel; host copy/OS overhead not modelled.

## p2f2_breakdown

P2-F2. W1 page recovery breakdown. (a) n_tiles=8; (b) n_tiles=32. The upper strip is resume (hatched); the lower panel is the total excluding resume, stacked as quiesce+restore+replay+publish. Resume is separated because it is work the no-fault job would also do. B0 totals are annotated. Black diamonds are L3 measured recovery_cycles_excl_resume.

- Sample rows: 2175
- Source: `results/phase2/v21/figure_data/`.
- ASSUMED: W4/MUL/RELU cost fits are L3-derived but not hardware-measured; MIX-A/MIX-B are illustrative; single channel; host copy/OS overhead not modelled.

## p2f3_release

P2-F3. No-fault publication overhead with PIM/host mode mutual exclusion. Host consumes only final outputs, so t_readable=max(t_release,t_exit); DEP_RELEASE is never negative. GLOBAL_HOLD uses H=Delta; DEP_RELEASE uses Delta. Black x are Phase 1 P1-F measured points and diamonds are E2E-9 L3 measurements (GLOBAL_HOLD H=T) with the calibrated host-read cost added so they share the L2 host-inclusive basis.

- Sample rows: 783
- Source: `results/phase2/v21/figure_data/`.
- ASSUMED: W4/MUL/RELU cost fits are L3-derived but not hardware-measured; MIX-A/MIX-B are illustrative; single channel; host copy/OS overhead not modelled.

## p2f4_radius

P2-F4. Radius policies at n_tiles=32. (a) Replayed tiles as a fraction of the job. (b) Expected silent-error coverage under two illustrative distance distributions: MIX-A same_row/±1/±2/far = 0.20/0.70/0.08/0.02, MIX-B = 0.10/0.50/0.30/0.10. The distributions are ASSUMED and do not correspond to a measured DRAM device.

- Sample rows: 5
- Source: `results/phase2/v21/figure_data/`.
- ASSUMED: W4/MUL/RELU cost fits are L3-derived but not hardware-measured; MIX-A/MIX-B are illustrative; single channel; host copy/OS overhead not modelled.

## p2f5_status

P2-F5. P2Status distribution over 30 seeds. DELTA_FULL_1.0 uses the true delay and now recovers selectively; DELTA_SHORT_PAGE_0.5 is fixed by BIRTH windows; DELTA_SHORT_BANK_0.5 still has 21/30 runs with wrong output despite SELECTIVE_RECOVERED, which is the remaining bank-granularity limitation. B0 is excluded from the status distribution.

- Sample rows: 10
- Source: `results/phase2/v21/figure_data/`.
- ASSUMED: W4/MUL/RELU cost fits are L3-derived but not hardware-measured; MIX-A/MIX-B are illustrative; single channel; host copy/OS overhead not modelled.

## p2f6_validation

P2-F6. L2 predictions versus all L3 W1/W2 fault-recovery cases. Calibration uses only no-fault L3 kernel timings at n={8,16,32}; the validation set is every normal recovery run (NO_REPLAY controls excluded). (a) recovery_cycles_excl_resume; (b) fault_overhead_cycles. MAPE is printed per workload; W2 is fully out-of-sample for the per-kernel fit.

- Sample rows: 1468
- Source: `results/phase2/v21/figure_data/`.
- ASSUMED: W4/MUL/RELU cost fits are L3-derived but not hardware-measured; MIX-A/MIX-B are illustrative; single channel; host copy/OS overhead not modelled.

## p2f7_late

P2-F7. Late-report sweep at n_tiles=32, page reports, UE, B1 with AUTO windows. (a) affected tile operations; (b) fault_overhead_cycles. F is the W4 fan-out. As D grows, more downstream readers have completed and the forward closure widens; W4_F16 rises fastest. E2E-8 confirms the same effect in the simulator (MUL_2 enters affected). W4 costs are ASSUMED from the MUL fit.

- Sample rows: 7204
- Source: `results/phase2/v21/figure_data/`.
- ASSUMED: W4/MUL/RELU cost fits are L3-derived but not hardware-measured; MIX-A/MIX-B are illustrative; single channel; host copy/OS overhead not modelled.
