# G7 Trusted Input Recovery and State Reset

G7 implements the P1-E recovery interface in the Phase 1 model. It performs a
whole-kernel recovery of W1 (FP16 ADD) after a CE report or an explicit
`RECOVERY_PROBE`:

```text
report or probe
  -> QUIESCE_REQUEST / drain / QUIESCE_ACK          (G6 path)
  -> lock old output version (SUSPECT or PRODUCING)
  -> discard old uncommitted external descriptors
  -> exit PIM mode to SB
  -> reset CRF, GRF/SRF, intermediate and control state
  -> restore immutable A/X from the trusted checkpoint
     through simulated host write transactions
  -> allocate a new output version
  -> re-execute the entire W1 kernel
  -> publish the new version
  -> HOST read and bit-for-bit verification
```

The recovery source is `Phase1CheckpointStore`, created from the initial A/X
vectors. The fixture never reads `reference.bin`; the expected output is used
only after the new version is read for the final comparison.

## 1. Interfaces and model assumptions

- `Phase1CheckpointStore` stores an immutable A/X copy and version, with an
  FNV-1a checksum used only for copy-integrity validation. It is a protected
  host-area model, not a cryptographic guarantee.
- `Phase1Runtime::quiesceOutput`, `beginRestore` and `beginReexecute` add the
  G7 transitions `PRODUCING/SUSPECT -> QUIESCED -> RESTORING` and
  `ALLOCATED/RESTORING -> REEXECUTING`. `PUBLISHED` is rejected with
  `ALREADY_COMMITTED`; old handles are rejected with `STALE_HANDLE`.
  Restore maintenance is performed while the old version is `RESTORING`; the
  new version is allocated afterward and enters `REEXECUTING` from
  `ALLOCATED`. This is a model-side version-allocation choice, not a claim
  about hardware output-buffer allocation.
- `Phase1FeederMemory::clearPending` discards the old kernel's unsubmitted
  descriptors. Transactions already accepted by the channel are drained by
  the G6 pause controller before ACK.
- `PIMRank::resetState` zeroes CRF, all PIM block GRF/SRF/intermediate values,
  control flags and the PIM program state, and sets the rank mode to `SB`.
  The mode transition is a direct model-side state reset, not a measured
  hardware PIM-exit command sequence.
- Restore writes are ordinary host write transactions issued through
  `MultiChannelMemorySystem::addTransaction` while the output version is in
  `RESTORING`. This bypasses the frozen feeder but still traverses the
  simulated controller/DRAM write path. Real host copy, interconnect and OS
  costs are excluded.
- The experiment injects one storage fault before the old execution and does
  not continue injecting after recovery. It validates the recovery interface;
  it does not claim isolation of a persistently vulnerable cell or protection
  against an ongoing Rowhammer attack.

## 2. Positive matrix

P1-E requires `n_tiles in {1,8}` and `seeds = 0..9`. The runner executes:

- 20 report-triggered cases: W1 with F1 single-bit injection in A, CE report
  triggers pause;
- 4 explicit-probe cases: `RECOVERY_PROBE` requests pause before old work is
  submitted; no storage fault is injected. These are labeled separately as
  interface probes.
- 2 deliberately broken recovery controls;
- 5 required negative-status cases.

Every positive case checks: old output not published, version change, stale
old handle, checkpoint bytes, restore write/byte counts, raw A/X storage after
restore, state reset from known non-default values, discarded old pending
work, drained re-execution, no new CE/UE, new version `PUBLISHED`, HOST read
allowed, raw B storage correctness, and C++ plus independent Python bit-exact
output comparison.

## 3. Results

### Report-triggered positive cases

| property | `n_tiles=1` (10 seeds) | `n_tiles=8` (10 seeds) |
|---|---:|---:|
| checkpoint bytes | 8192 | 65536 |
| restore host writes | 256 | 2048 |
| restore bytes | 8192 | 65536 |
| discarded old pending descriptors | 68 | 446 |
| CE report cycle | 1305 | 8938 |
| request -> ACK cycle | 1313 -> 1354 | 8946 -> 8987 |
| quiesce cycles | 41 | 41 |
| restore loop cycles | 557 | 4274 |
| re-execution loop cycles | 804 | 4130 |
| ACK -> publish cycles | 1361 | 8404 |
| output version | 1 -> 2 | 1 -> 2 |
| final bit-exact | 10/10 | 10/10 |

For all 20 report cases, the old output was `SUSPECT` and not published
before restore. The mode before reset was `HAB_PIM`; after reset it was `SB`.
All restored raw A/X codewords matched the checkpoint, and the re-executed
output had no new CE/UE reports.

### Explicit `RECOVERY_PROBE` cases

| property | `n_tiles=1` (seeds 0,1) | `n_tiles=8` (seeds 0,1) |
|---|---:|---:|
| discarded old pending descriptors | 103 | 481 |
| ACK cycle | 1049 | 8338 |
| restore loop cycles | 557 | 4322 |
| re-execution loop cycles | 804 | 4094 |
| ACK -> publish cycles | 1361 | 8416 |
| old report count | 0 | 0 |
| final bit-exact | 2/2 | 2/2 |

The probe cases pause while the old output is still `PRODUCING`; no old work
is submitted before the freeze. This demonstrates the explicit recovery
probe path independently of a CE report.

## 4. Required negative statuses

The runner starts one simulator process per negative case. Each case writes
the actually observed status, not a hard-coded pass value.

| case | observed status | evidence |
|---|---|---|
| checkpoint absent | `CHECKPOINT_MISSING` | `runs/g7/negative_statuses/CHECKPOINT_MISSING/metrics.json` |
| expected version 2 vs stored 1 | `VERSION_MISMATCH` | `runs/g7/negative_statuses/VERSION_MISMATCH/metrics.json` |
| corrupted checkpoint copy | `CHECKPOINT_CORRUPT` | `runs/g7/negative_statuses/CHECKPOINT_CORRUPT/metrics.json` |
| restore on published output | `ALREADY_COMMITTED` | `runs/g7/negative_statuses/ALREADY_COMMITTED/metrics.json` |
| old consumer handle after new version | `STALE_HANDLE` | `runs/g7/negative_statuses/STALE_HANDLE/metrics.json` |

The `STALE_HANDLE` case also records `old_can_consume=false` and
`current_version=2`.

## 5. Negative controls for necessary initialization

Two broken-recovery controls are expected to be caught:

1. `NO_INPUT_RESTORE` (n_tiles=1, seed=0): the restore writes are skipped.
   The original F1 bit remains in raw A storage
   (`restored_raw_a_mismatches=1`), re-execution produces 2 CE reports, the
   new output stays `SUSPECT`, the HOST read is blocked and the output is not
   published. This shows that the restore step is not a no-op and cannot be
   replaced by "restore returned true".
2. `NO_STATE_RESET` (n_tiles=1, seed=0): `resetState()` is skipped. The rank
   remains in `HAB_PIM` (`mode_before_reset=HAB_PIM`,
   `mode_after_reset=HAB_PIM`), so the simulated host restore writes cannot
   complete within the 10000-cycle watchdog (`restore_timeout=true`). The
   case is caught before a new version is published.

The second control catches the missing control-state reset through the
restore path. It does not claim that stale GRF/SRF values alone change the
FP16 ADD numerical result: the ADD kernel reloads GRF A from A storage and
programs the CRF, so the observed failure is the stale PIM mode blocking the
recovery writes.

## 6. State reset witness

Before reset, the fixture writes known non-default values into:

- `GRF A[0]`, `GRF B[0]`;
- `SRF`;
- `mOut` and `aOut` intermediate bursts;
- CRF word 0;
- implicitly, the PIM control/mode state left by the interrupted execution.

The report-triggered cases enter reset with `mode_before_reset=HAB_PIM`; after
reset, `zeroPimState()` is true and `mode_after_reset=SB`. This is recorded as
`nondefault_before_reset=true`, `state_reset_count=1`, `reset_ok=true`.

## 7. G7 runner and evidence

```text
scripts/run_g7.py
results/g7/summary.csv
results/g7/validation.json
runs/g7/run_manifest.json
runs/g7/positive/...
runs/g7/probe/...
runs/g7/negative_control/...
runs/g7/negative_statuses/...
runs/g7/clean_replay/...
logs/g7_build.log
logs/g7_run.log
logs/g7_clean_smoke.log
logs/g7_final_audit.log
```

The runner's 13 aggregate checks all pass: 20 report cases, 4 probe cases,
2 caught negative controls, 5 status negatives, independent bit-exact output
for every positive case, version changes, expected restore bytes, both
trigger sources labeled, and no simulator failures.

## 8. Boundaries

- This is a model-side recovery interface, not real HBM-PIM hardware support.
- Checkpoint protection, host copy cost, interconnect cost and OS notification
  are not modeled.
- Restore uses direct host write transactions through the simulated memory
  system; it is not routed through the frozen external feeder.
- PIM mode exit is a direct state reset; detailed command-level PIM exit and
  re-entry timing are not measured.
- The fault is one-shot; continued attack/migration policy is out of scope.
- Positive probe cases are interface probes without storage injection and are
  not counted as additional Rowhammer samples.
- The old output is discarded, not repaired in place. A committed output is
  rejected with `ALREADY_COMMITTED`; G7 does not claim original-output
  integrity recovery after commit.
