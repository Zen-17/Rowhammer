# M2-R refresh-scheduling fix

G3 stays REVISE. M3 was not started. E1, E2, and E4 were not rerun. Threshold, tREFI, tRFC, tRAS, tRP, attack caps, and swap periods were not changed.

## Root cause

`CommandQueue::process_command` had the refresh stall commented out. While `refreshWaiting` was set, a new row hit or ACT was still issued. A write to an open row sets `nextPrecharge = max(nextPrecharge, now + WRITE_TO_PRE_DELAY)`, so the PRE that `process_refresh` needs never became legal and REF was starved.

`Phase3M2Test::drain` stopped when the host transaction completed. A write clears `numOnTheFlyTransactions` when the data burst finishes, while the row stays RowActive and `Rank::refreshWaiting` can still be true. The clock stopped before tRAS elapsed, and the next attack pair extended `nextPrecharge` again.

That is why NONE A1 previously completed 5 REF commands in 5,272,400 cycles.

## Change

- `pim-phase1/simulator/src/CommandQueue.cpp`: `process_command` returns immediately when `refreshWaiting` is true. `process_refresh` still issues PRE once `currentClockCycle >= nextPrecharge`, then REF after every bank is idle. `pop` already tries refresh first.
- `phase3_exp/sim_hooks/Phase3M2Test.cpp`: `drain` keeps ticking while any rank has `refreshWaiting`. The per-pair attack/background enqueue is unchanged: one `HOST_ATK` and one `HOST_BG` per step.

## Verification

`Phase3M2R.AlternatingDrainProducesOneActPerAccess` still passes: 32 alternating steps produce at least 30 ACTs.

`Phase3M2R.RefreshPreemptsOpenRowAndClearsDisturbance` passes: 400 open-row hammer steps, REF count within 2 of `cycles/3900`, and D(row 3) is strictly below the neighbor ACT count, so a REF cleared the row.

## E3 rerun

New files only: `results/m2r_refresh_fix/E3_real_ref_on.csv`, `runs/M2R_refresh_fix/`. Real HC 14531, REF_ON, W2, one seed each. Previous `results/m2r_attack_fix/` was left in place.

| scheme | attack | cycles | REF | REF / (cycles/3900) | injections | first crossings | residual | peak D/threshold |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| NONE | A1 | 5785429 | 1470 | 0.991 | 4 | 1 | 4 | 1.447 |
| NONE | A2 | 49615800 | 12708 | 0.999 | 2 | 1 | 2 | 0.518 |
| NONE | A3 | 128252 | 19 | 0.578 | 0 | 0 | 0 | 0.00197 |
| NONE | A4 | 8114487 | 2067 | 0.994 | 7 | 1 | 7 | 1.658 |
| FIX | A1 | 4236407 | 1073 | 0.988 | 0 | 0 | 0 | 0.0287 |
| FIX | A2 | 57079242 | 14622 | 0.999 | 0 | 0 | 0 | 0.0887 |
| FIX | A3 | 134467 | 21 | 0.609 | 0 | 0 | 0 | 0.00122 |
| FIX | A4 | 9003944 | 2295 | 0.994 | 0 | 0 | 0 | 0.00199 |
| ADA | A1 | 6245943 | 1588 | 0.992 | 0 | 0 | 0 | 0.0159 |
| ADA | A2 | 25388746 | 6496 | 0.998 | 0 | 0 | 0 | 0.0342 |
| ADA | A3 | 128567 | 19 | 0.576 | 0 | 0 | 0 | 0.00108 |
| ADA | A4 | 4690351 | 1189 | 0.989 | 0 | 0 | 0 | 0.0131 |
| OURS | A1 | 6370436 | 1620 | 0.992 | 0 | 0 | 0 | 0.0262 |
| OURS | A2 | 29395958 | 7524 | 0.998 | 0 | 0 | 0 | 0.0712 |
| OURS | A3 | 128252 | 19 | 0.578 | 0 | 0 | 0 | 0.00176 |
| OURS | A4 | 9158242 | 2335 | 0.995 | 0 | 0 | 0 | 0.0178 |

NONE A1 `neighbor_acts` is 104010 against 104009 attack transactions, so the per-step drain still issues one activation per alternating access. The host mix is still one attack write and one background write per step.

NONE still flips on A1, A2, and A4 at the real threshold with refresh on. A2's sampled peak (0.518) is below 1 because the sample is taken after `drain`, and a REF inside that drain can clear D after the crossing ACT. The injection count is the flip record: 2 placed, 1 first crossing, 2 residual.

A3 remains at the locked cap of 1024 attacks. No scheme flips. That is the attack definition, not a defense result. The short A3 run also issues fewer REFs per cycle (about 0.58 of `cycles/3900`) because the kernel burst is a larger fraction of the run; the long hammer runs sit at 0.99.
