# G5 Output State and Consumer Gates

G5 adds the output-version state machine and host/PIM consumer admission gates.
It does not implement G6 pause/quiesce or G7 restore.

## 1. Runtime model

`Phase1Runtime` tracks one managed producer output version:

```text
ALLOCATED -> PRODUCING -> READY_UNPUBLISHED -> PUBLISHED
                   \              \
                    -> SUSPECT <---
                    -> POISONED
```

- A CE report locks a non-published output as `SUSPECT`.
- A UE report locks it as `POISONED`.
- `SUSPECT` and `POISONED` never become `PUBLISHED` by timeout.
- `H=0` allows publish at the first post-completion tick if no report wins;
  `H>0` delays publish by at least H DRAM cycles.
- A report after `PUBLISHED` is recorded as a late report; G5 does not revoke
  the already published version.
- A new output version invalidates the old handle; old handles return
  `ABORTED`/stale and fail the consumer gate.

The output data remains in the existing HBM result region. The runtime stores
version/gate metadata, not a second full copy of the result.

## 2. Gates

- Host consumer: a read of output data is admitted only in `PUBLISHED`.
- PIM consumer: submission is admitted only in `PUBLISHED`.
- Producer internal reads/writes are not blocked by its own output gate; W1
  executes while the output version is `PRODUCING`.
- `assertConsumerAllowed()` is the test/evaluator bypass guard: it throws
  `STALE_HANDLE` or `UNPUBLISHED_OUTPUT:<state>` rather than waiting inside
  `Bank::read()`.

The public report fields are the only input to `onReport`; the runtime has no
fault schedule, truth, golden output or exact storage key.

## 3. G5 smoke

12 W1 cases: `fault ∈ {F0,F1,F2} × hold ∈ {0,8} × consumer ∈ {HOST,PIM}`, n_tiles=1.

| fault | state after hold | published | host/PIM gate | reports |
|---|---|---|---|---|
| F0 | PUBLISHED | yes | admitted | none |
| F1 | SUSPECT | no | blocked | one CE |
| F2 | POISONED | no | blocked | one UE |

- F0 HOST output is bit-exact against the independent A+X reference.
- F1/F2 output files are empty because the consumer gate blocks the read; no
  consumer data is fabricated.
- F1/F2 `bypass_assert=true`; host and PIM gates both block.
- PIM consumer F0 is admitted by `canAdmit`, but G5 does not implement W2; no
  PIM consumer data is produced.

`Phase1Runtime.SameCycleAndStaleHandles` independently checks:

1. report one cycle before publish -> SUSPECT;
2. report in the publish cycle before tick -> SUSPECT;
3. report one cycle after publish -> PUBLISHED + late report counter;
4. old handle after a new version -> stale/ABORTED;
5. unpublished host/PIM gates false and bypass guard throws.

Evidence: `runs/g5/`, `results/g5/`, `logs/g5_*.log`.

## 4. Boundaries

- No quiesce, ACK, in-flight drain, pause, restore or re-execute.
- No P1-C full matrix, T_ref calibration, H ratio sweep, or W2 consumer kernel.
- `SUSPECT`/`POISONED` are terminal for G5; G6/G7 will add the later states.
- The latency and hold cycles are model configuration, not hardware timing.
