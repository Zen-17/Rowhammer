| Metric | v2 | v2.1 |
|---|---|---|
| DELTA_FULL_1.0 status | PARTIAL_WITH_ESCAPE 30/30 | SELECTIVE_RECOVERED 30/30 |
| DELTA_SHORT_0.5 (page) status/output | SOURCE_REPAIRED_ONLY, wrong | SELECTIVE_RECOVERED, correct 30/30 |
| DELTA_SHORT_0.5 (bank) status/output | not run | 21/30 SELECTIVE wrong, 9/30 fallback correct |
| W1 bank B1 replay fraction | 46.4% | 9.55% (D-averaged) |
| W1 bank B1 / B0 fault overhead | 104.3% (recovery) | 18.1% (fault overhead) |
| E2E-3 restore_bytes / affected | 65,536 B / 5 tiles | 32,768 B / 3 tiles |
| DEP_RELEASE overhead at Delta=0 | -36% | 0.01% (model, no negative) |
| W2 page UE affected fraction | 1.04% (D-averaged) | 0.35% / 1.39% / 1.39% at D=16/256/4096 |
| W2 validation MAPE | 22.43% (recovery) | 11.89% (fault overhead) / 21.16% (recovery excl. resume) |
| F2 cases classified MASKED | not measured | 160/360 = 44.4% |
| B0 status label | WHOLE_JOB_FALLBACK | BASELINE_B0 |
| L2 cases | 47,946 (report) / 48,666 actual | 59,706 |
| L3 runs | 745 | 1,120 |
