# G4 All-Path ECC and Coarse Reports

G4 integrates the SECDED model into the host and PIM storage paths and adds a
coarse public report queue. It does not implement the G5 output gate or the G6
pause/restore state machine.

## 1. Access paths and profiles

`Bank` now receives an explicit `Phase1AccessType`:

```text
HOST, PIM_OPERAND, PIM_RESULT, HAB
```

The call sites are:

| path | access type | source |
|---|---|---|
| host read/write | HOST | `Rank::readSb/writeSb` |
| PIM bank→GRF/HAB transfer | HAB | `PIMRank::readHab` |
| PIM result writeback | PIM_RESULT | `PIMRank::writeHab`, `writeOpd`, NOP write |
| PIM operand/result bank access | PIM_OPERAND / PIM_RESULT | `PIMRank::readOpd/writeOpd` |

Profiles:

- `NATIVE`: original raw `rowEntries` behavior; no parity, detection or report.
- `LOCAL_VISIBLE`: all access types protected; CE/UE detected and public reports enabled.
- `LOCAL_HIDDEN`: all access types protected; CE detected internally but not
  enqueued as public; UE remains reportable for the later runtime semantics.
- `HOST_ONLY`: HOST accesses protected; PIM/HAB reads bypass the decoder and
  PIM/HAB writes use `writeUnprotected` (`ecc_valid=false`). A later host read
  of such a codeword returns `UNPROTECTED_FORMAT`, not CE/UE.

The protected bank storage uses `Phase1Storage` with a key containing
`(channel, rank, actual_bank, row, burst_column, codeword_index)`. The host and
PIM callers pass the actual bank index; PIM broadcast paths use the actual bank
loop in `PIMRank`, so a report is not built from `packet->bank`.

ECC pipeline latency is not modeled (zero additional cycles), as required for
this interface/timing phase.

## 2. Report queue

`Phase1Reports` exposes only:

```text
report_seq, report_cycle, error_class, region_type, region_id
```

The internal detection record (detect cycle, actual location, codeword, access
type) is not written to `reports.csv` and is not visible to the report file.
Reports are enqueued FIFO; `ready_cycle = detect_cycle + max(1,D)`, service
interval `I`, capacity `Q=64`. Overflow increments a drop counter and sets an
overflow flag; G4 does not send an extra overflow warning.

Region:

- `bank`: `channel * NUM_BANKS + actual_bank`.
- `page`: the Scheme8 host physical address from
  `(channel,rank,actual_bank,row,burst_column)` divided by 4096.

A page can contain multiple banks; bank and page are separate labels, not a
strict hierarchy.

## 3. Smoke results

### P1-A

144 cases: `4 profiles × 4 paths × {F0,F1,F2} × {bank0,bank3,bank15}`.
All 144 passed. Aggregate counts:

| profile | cases | internal CE | internal UE | delivered | format-invalid cases |
|---|---:|---:|---:|---:|---:|
| NATIVE | 36 | 0 | 0 | 0 | 0 |
| LOCAL_VISIBLE | 36 | 12 | 12 | 24 | 0 |
| LOCAL_HIDDEN | 36 | 12 | 12 | 12 (UE only) | 0 |
| HOST_ONLY | 36 | 3 | 3 | 6 | 9 |

- LOCAL_VISIBLE F1: all four paths corrected to the original data and delivered
  a CE (bank 0/3/15).
- LOCAL_HIDDEN F1: corrected data but no public CE; UE cases remain reported.
- HOST_ONLY: HREAD host correction works; PREAD/PWRITE_PREAD bypass correction
  and produce no ECC report; PWRITE_HREAD marks the result `UNPROTECTED_FORMAT`
  and produces four format events (one per codeword), not CE/UE.
- NATIVE: no ECC/report; F1/F2 change the raw output.
- Reports were checked to point to the actual target bank (0/3/15), not a
  broadcast packet bank.

### P1-B

63 cases: LOCAL_VISIBLE bank/page, banks 0/3/15, D ∈ {0,1,16,256,4096},
I ∈ {1,8}, plus 32-read burst cases.

- Single-read latency is exactly `max(1,D)` for every D (1, 1, 16, 256, 4096).
- All single-event cases deliver one CE, no drops, and preserve output bits.
- The 32-read burst cases produce 32 actual CE reports with no drops; the
  observed service gaps are at least I=8. The queue peak is below Q=64.

The latency values are model/configuration outputs, not hardware notification
measurements.

## 4. Evidence

```text
runs/g4/p1a/...
runs/g4/p1b/...
runs/g4/clean_replay/...
results/g4/p1a_summary.csv
results/g4/p1b_summary.csv
results/g4/summary.csv
results/g4/validation.json
logs/g4_*.log
```

`runs/g4/truth.csv` records the evaluator-side INJECTION row (raw before/after,
target bank/codeword/bit positions); the runtime/public path does not read it.

## 5. Boundaries

- G4 uses targeted host/PIM storage paths and a direct PIMRank access path for
  the P1-A smoke, not the full paper matrix.
- No output gating/publish state, pause/restore, or selective re-execution.
- No claim about commercial Aquabolt-XL ECC layout, pipeline cost, or hardware
  notification latency.
- `PIMBenchFixture.add` remains the known G0 performance-threshold failure.
