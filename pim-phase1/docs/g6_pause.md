# G6 Pause, Drain and Two-Channel Scope

G6 implements a whole-channel quiesce primitive. It stops new feeder
submissions for the target channel, lets accepted transactions drain, and
issues `QUIESCE_ACK` when the per-channel data path is idle. It does not stop
the DRAM clock or refresh, and does not implement G7 restore.

## 1. Protocol

```text
QUIESCE_REQUEST
  -> freeze target channel in Phase1FeederMemory
  -> no new external descriptors admitted for target channel
  -> accepted/command/rank data returns drain
  -> QUIESCE_ACK
```

- External pending descriptors remain frozen and are not counted as drain work.
- The target channel's `Bank` data-access counter must not increase after ACK.
- The simulator clock and REF continue.
- Other logical channels remain eligible for feeder submission.
- The pause scope is whole channel; no bank-level pause is claimed.

`Phase1PauseController` supports:

- direct `request(cycle, reason)` for synthetic control probes;
- `notifyReport(report_cycle)` with handler delay L for a real CE report;
- `tick(cycle)` to request/ack quiesce and record quiesce cycles.

## 2. Synthetic probes and actual CE

16 cases:

- 4 pause points × feeder limit K ∈ {1,8,32}:
  - `SYNTHETIC_BEFORE`: before first submission;
  - `SYNTHETIC_QUEUE`: feeder queue nonempty, target not yet issued;
  - `SYNTHETIC_INFLIGHT`: accepted/in-flight data work;
  - `SYNTHETIC_NEAR`: near kernel completion.
- Actual CE: W1 producer, F1 injected in the first A codeword; a real public CE
  report triggers request after L ∈ {0,8,64}.
- Two-channel: independent host transactions on channel 0 and 1; pause channel 0,
  continue channel 1.

All cases use an 8000-cycle post-ACK observation window. The minimum required
window (`max(32, 2*tRFC)`) is 700 cycles for this device model.

## 3. Results

- All 16 cases ACKed with no timeout.
- `data_delta_after_ack = 0` in every case.
- Clock advanced 8000 cycles in every observation; refresh counters did not
  regress, and each observed window increased them (0 -> 2 or more where
  applicable).
- Synthetic cases left external descriptors frozen; e.g. before/queue had 2529
  and 2528 pending descriptors.
- Actual CE:
  - report at 1305; L=0/8/64 request at 1305/1313/1369;
  - ACK at 1354/1354/1422;
  - no data access after ACK; external descriptors remained frozen.
- Two-channel:
  - channel 0 data accesses stopped at 8 and stayed 8;
  - channel 1 advanced from 8 to 64 during the same observation;
  - 56 channel-0 descriptors remained frozen.
- The 2-channel case uses independent host traffic, not a full W1/W3 kernel
  pair; no claim of zero cross-channel performance impact is made.

## 4. Evidence

```text
runs/g6/synthetic/...
runs/g6/actual_ce/...
runs/g6/two_channel/...
results/g6/summary.csv
results/g6/validation.json
logs/g6_*.log
```

## 5. Boundaries

- No QUIESCED -> RESTORING / REEXECUTING transitions (G7).
- No selective re-execution, checkpoint restore or old-result recovery.
- The pause is channel-scoped; no bank-level pause.
- The performance effect is not claimed zero; only the channel-1 progress is
  reported.
- No hardware pause latency is claimed: L and observation windows are model
  configuration.
