# Stage 4–5 simulator patches

`phase3_exp/build/` is gitignored.  Stage 4–5 edits were applied to the
generated tree `build/simulator/src/` and were **not** present in
`pim-phase1/`, `pim-phase2/`, or `apply_m2_hooks.py`.

`build_phase3.sh` starts with `rm -rf "$BUILD"`.  Running it without the
overlay below would delete `moveCodeword`, `identityBank`,
`mapping_consistent`, `P2Status::STALE_EPOCH`, `Ledger::observeAccess` /
`completeObserved`, `phase3_operand_denied`, and epoch-bound request
stamping.  `Phase3M2Test.cpp` already calls all of those symbols.

Stop-loss:

1. `stage4_overlay/` holds the 19 patched sources taken from the live
   build tree on 2026-11-26.
2. `apply_stage4_hooks.py` copies that overlay onto a freshly assembled
   build tree **after** the M2/rank/GEMV hooks.
3. `stage4_vs_hooks.diff` is the unified diff of those 19 files against
   a tree reconstructed from `pim-phase1` + `pim-phase2` + the previous
   hooks (no Stage 4–5).

Do not treat `build/simulator/src` as source of truth.  New simulator
behavior belongs in `sim_hooks/` (this overlay or a later hook script).
