# Phase 2 Mount Points — Stage 1 verification

Phase 2 root: `$P2` = this `pim-phase2` directory.
Shared Phase 1 root: `$P1` = `../pim-phase1` (the directory containing
`simulator/Sconstruct`, `scripts/run_suite.py` and `README.md`).
Verified revision: `274a07a86b90c45a528adf162e1b77c2fbd016fe` (Phase 1
workspace) plus the untouched fixed upstream simulator sources.

References below to `simulator/src/...` describe the Phase 1 source facts;
the Phase 2 build tree is generated at `$P2/build/simulator` from
`$P1/simulator` plus `$P2/overlay/src`. Phase 2 owns only the overlay files
listed in §3.

This document records where Phase 2 attaches to Phase 1. Every row of the
Phase 2 plan's fact table was checked against the working copy; no fact was
found to be wrong. Additions are listed at the end.

## 1. Fact table

| # | Topic | Fact checked | Result | Evidence |
|---|---|---|---|---|
| 1 | Report interface | `Phase1Reports::tick(cycle)` delivers at most one report per cycle; `reports()` returns `std::vector<Phase1PublicReport>`; fields `report_seq, report_cycle, error_class("CE"/"UE"), region_type("bank"/"page"/"codeword"), region_id`; also `overflow()`, `dropped()` | 一致 | `simulator/src/Phase1Reports.h`, `Phase1Reports.cpp::onDetected/tick` |
| 2 | region_id encoding | bank = `channel*num_banks + actual_bank`; page = `hostAddress/4096`; codeword = `(((ch*num_banks+bank)*num_rows+row)*burst_columns+burst_col)*4+cw` | 一致 | `Phase1Reports.cpp::onDetected`; geometry from `Phase1Address` |
| 3 | Report queue construction | `Phase1Reports(codec, num_channels, num_banks, granularity, transport_delay, service_interval, capacity, deliver_ce, deliver_ue)` | 一致 | `Phase1Reports.h` constructor |
| 4 | ECC profile | `bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &reports)`; CE corrected on the read path, PIM receives corrected data; UE delivers uncorrected data | 一致 | `Bank.cpp::eccEnabled/read`; `Phase1G5TestCases.cpp`; `report/phase1_results.md` |
| 5 | Fault injection | `Bank::injectRawDataBit(row, burstColumn, bitInBurst)`; F1 (CE) = bit 15; F2 (UE) = bits 0 and 1 of one codeword | 一致 | `Bank.cpp`; `Phase1G5TestCases.cpp` fault block |
| 6 | Pause / drain | `Phase1PauseController`: `notifyReport(report_cycle)`, `tick(cycle)`, `acked()`, `ackCycle()`, `request(cycle, reason)`; `runFeederLoop` drives it | 一致 | `Phase1Pause.h`, `Phase1Pause.cpp`, `Phase1G7TestCases.cpp::runFeederLoop` |
| 7 | Feeder | `Phase1FeederMemory::freezeChannel/unfreezeChannel/clearPending/isIdle/pendingCount`; completion callbacks already exist for its own accounting | 一致 | `Phase1Feeder.h`, `Phase1Feeder.cpp` |
| 8 | State reset | Mid-recovery rank is in `HAB_PIM`; `PIMRank::resetState()` returns it to `SB`; without reset host writes cannot complete | 一致 | `PIMRank.cpp::resetState`; `Phase1G7TestCases.cpp` NO_STATE_RESET control; `docs/g7_recovery.md` §5 |
| 9 | Recovery write | `feeder->MultiChannelMemorySystem::addTransaction(true, addr, &burst)` then `addBarrier(0)` and `runDirectWrites` | 一致 | `Phase1G7TestCases.cpp` restore block |
| 10 | W1 workload | `preloadNoReplacement(&A,0,0)`, `(&X,128,0)`; `executeEltwise(dim, ALL_BANK, ADD, 0, 256, 128)`, `dim=n_tiles*128` bursts, output row 256 | 一致 | `Phase1G7TestCases.cpp`; `PIMKernel.cpp` |
| 11 | Tile layout | tile i uses burst columns `8i..8i+7` on all 16 banks via ALL_BANK; `addrGenSafe` folds column overflow into row; exactly one 4 KiB host page per tile (page = row*4 + col/8) | 一致 | `PIMKernel.cpp::computeAddOrMul`; `KernelAddrGen.cpp::addrGenSafe`; Scheme8 arithmetic checked |
| 12 | Broadcast bank identity | real bank = `pb*2 + packet->bank`; `packet->bank` is only the even/odd offset | 一致 | `PIMRank.cpp::readHab/writeHab`; `docs/source_audit.md` §2 |
| 13 | Supported kernels | `executeEltwise` supports ADD, MUL, RELU; each kernel pays parkIn→SB→HAB→CRF→HAB_PIM→compute→parkOut | 一致 | `PIMKernel.cpp::executeEltwise` |
| 14 | Output gate | `Phase1Runtime` tracks a single overall output version; any report locks it; `beginRestore` on PUBLISHED throws `ALREADY_COMMITTED` | 一致 | `Phase1Runtime.cpp` L104-105, L123-129 |
| 15 | Checkpoint | `Phase1CheckpointStore` stores immutable W1 A/X copies only | 一致 | `Phase1Checkpoint.h/.cpp` |
| 16 | Fixture convention | Phase 1 fixture reads `PHASE1_CONFIG/PHASE1_OUT`; runner executes `./sim --gtest_filter=<Fixture>.RunCase` with `cwd=simulator/` | 一致 | `Phase1G7TestCases.cpp`; `scripts/run_suite.py::run_one` |
| 17 | Calibration data | `results/g7/summary.csv` positive rows; `results/figure_data/fig4_quiescence.csv`; `fig6_cost.csv` columns as stated | 一致 | `scripts/phase2/calibrate.py` output `configs/phase2/calibration.json` |
| 18 | Calibration reference | n_tiles 1/8: restore 557/4274, reexec 804/4130, ACK→publish 1361/8404; quiesce 41; P1-F H=0.25T 10.21%/H=1T 43.96% | 一致 | measured medians; calibration fit gives c_tile=475.14, c_fix=328.86, 0.064819 B/cycle, 26.0, 41.0; P1-F values are in `fig6_cost.csv` |

## 2. Differences

No substantive difference was found between the fact table and the code.

Notes that matter for implementation:

1. `results/g7/summary.csv` does not contain `quiesce_cycles`; the quiesce
   calibration therefore reads `results/figure_data/fig4_quiescence.csv`
   (`scenario == ACTUAL_CE`, median = 41). This is recorded in
   `configs/phase2/calibration.json`.
2. The feeder completion map `completion_ids_` is only populated when the
   trace is enabled. The new Phase 2 observer in `Phase1Feeder` is therefore a
   separate, always-on (when installed) read-only callback; it does not depend
   on tracing.
3. `Phase1CheckpointStore` only carries A/X. W2's Y input is also an INITIAL
   input; Phase 2 keeps its own trusted host copy (`phase2::CheckpointView`),
   because the plan requires restore of `Y` too (W2 fault positions include
   `{A,Y,T}`). This is a Phase 2 extension, not a Phase 1 change.
4. `Phase1Reports` delivery is at most one report per cycle with
   `service_interval`; the Phase 2 fixture uses the same class and the same
   `transport_delay`/`service_interval` semantics.

## 3. Mount points used by Phase 2 (read-only use unless stated)

| Mount point | Phase 2 use | Change to Phase 1 file |
|---|---|---|
| `Phase1Reports` (sink) | report generation/delivery; Phase 2 fixture keeps its own queue instance and does **not** forward reports to `Phase1Runtime::onReport` | none |
| `Phase1PauseController` | report-triggered channel quiesce and ACK timing | none |
| `Phase1FeederMemory` | preload, kernel execution, freeze/unfreeze, direct restore writes, idle checks | add `setCompletionObserver` only |
| `PIMKernel::preloadNoReplacement` | load A/X/Y/T/U/O operand tiles | none |
| `PIMKernel::executeEltwise` | full-kernel B0 path and Phase 1 baseline | none |
| `PIMKernel::executeEltwiseTiles` | selective partial replay of contiguous tile runs | new method only |
| `Bank::setPhase1(LOCAL_VISIBLE,&reports)` | CE/UE visibility and report generation | none |
| `Bank::injectRawDataBit` | F1/F2 fault injection | none |
| `Phase1CheckpointStore` | B0/W1 trusted A/X copy | none |
| `Phase1Address` | Scheme8 encode/decode; page and codeword report footprints | none |
| `PIMRank::resetState` | mandatory rank state reset before restore writes | none |
| `Phase1Runtime` | B0 baseline only; Phase 2 uses `phase2::Gate` | none |
| `Sconstruct` | auto-globs `src/*.cpp` and `src/tests/*.cpp`; no edit needed | none |

## 4. Address arithmetic used by Phase 2 (1 channel, 1 rank, HBM2 2M 16B x64)

With `NUM_BANKS=16`, `NUM_COLS=128`, `BL=4`, 32 B per burst,
`burst_columns=32`, `NUM_ROWS=16384`, one channel and one rank:

```text
addr = ((((row << 5) | burst_col) << 2 | bank_group) << 2 | bank_low) << 5
page = addr >> 12 = row*4 + burst_col/8
```

A 4 KiB page therefore contains 16 banks x 8 consecutive burst columns, which
is exactly one tile footprint. A tile has 128 bursts (16 banks x 8 columns) =
4096 bytes, consistent with the plan's `restore_bytes == 4096` for one tile.
