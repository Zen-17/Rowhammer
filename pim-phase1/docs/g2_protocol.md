# G2 Data Protocol and Event Trace

G2 adds passive event recording and the run-directory data protocol. It does not
implement ECC, fault injection, reports, output gating, pause or recovery.

## 1. Run configuration

`configs/schema.json` is the JSON Schema for the flat Phase1 run configuration.
The C++ `Phase1Driver` reads the corresponding `params.ini`; the schema is
checked by `scripts/validate_run.py` and `tests/test_schema.py`.

Required G2 keys include:

```text
schema_version, experiment, workload, mode, trace_mode,
channels, ranks, capacity_mb, n_tiles, seed,
feeder_outstanding_limit, feeder_issue_per_cycle, watchdog_cycles,
device_ini, system_ini, output_dir
```

G2 uses `trace_mode=ext_off` or `trace_mode=ext_on`. `ext_off` creates a
header-only `events.csv`; `ext_on` records events. Both use the same feeder and
memory path.

## 2. Event CSV

Header:

```text
event_seq,cycle,phase,event_type,kernel_id,version_id,access_id,channel,rank,
actual_bank,row,column,access_path,bytes,status
```

- `event_seq` starts at 1 and increases by exactly one.
- `cycle` is the upstream DRAM cycle (`MultiChannelMemorySystem::currentClockCycle`)
  at the event point; it never decreases.
- `phase` order per cycle is
  `ENQUEUE < START < FEEDER < MEMORY < COMPLETE < FINALIZE`.
- G2 event types are `DESCRIPTOR_ENQUEUE`, `BARRIER_ENQUEUE`,
  `CONTROLLER_ACCEPT`, `CONTROLLER_REJECT`, `BARRIER_RETIRED`, `READ_COMPLETE`,
  `WRITE_DATA_DONE`, `RUN_START`, `RUN_END`.

Missing values are empty fields, not zeros. G2 uses `kernel_id=1` and
`version_id=1` because it runs one unversioned W1 kernel; these are protocol
placeholders, not a Phase1 runtime output-version state.

### 2.1 Location fields and PIM broadcast

The feeder records the address-mapped channel/rank/bank/row/burst-column for
ordinary descriptors. For PIM broadcast tags (`BANK_TO_GRF_`, `ADD`,
`GRF_TO_BANK`, etc.), there is no single actual bank: the packet bank field is
only the even/odd selector and the PIM rank touches all PIM blocks. Such rows
have `actual_bank` empty and `access_path` ending in `:PIM_BROADCAST`.

PIM control writes (`PROGRAM_CRF`, `PIM`) have no ordinary data bank and are
marked `:PIM_CONTROL` with `actual_bank`, `row` and `column` empty. This avoids
presenting the packet bank field or a reserved control row as an ordinary data
location.

This is a G2 trace convention. It does not claim that a real HBM-PIM device
exposes this CSV or these fields.

## 3. Protocol files

Every G2 run directory contains:

```text
params.ini, config.json, metadata.json, stdout.log, stderr.log, exit_code,
events.csv, reports.csv, truth.csv, output.bin, reference.bin,
timing.json, metrics.json, validation.json
```

- `reports.csv` has exactly
  `report_seq,report_cycle,error_class,region_type,region_id` and is header-only
  in G2; no report interface is implemented.
- `truth.csv` has the full future-facing header including `record_type` and
  `detection_id` and is header-only in G2; no fault/injection exists.
- `metadata.json` contains run_id, upstream commit, patch/binary/config/environment
  hashes, start time, wall time, exit code and run status.
- `metrics.json` contains the required metric keys; undefined values are null,
  not fabricated zeroes. `producer_cycles` and `end_to_end_cycles` are the
  measured update count.
- `validation.json` lists every assertion with `name`, `pass`, `expected`,
  `actual` and `event_seq` (null where no single event applies).

## 4. EXT_OFF behavior check

`scripts/run_g2.py` runs feeder W1 n_tiles=1 and 8 with `ext_off` and `ext_on`:
- output bytes are identical between modes;
- cycle counts are identical;
- both match the G1 feeder canonical output/cycles;
- `ext_off` events.csv has only the header;
- `ext_on` events pass schema, sequence, cycle and phase checks.

Evidence: `results/g2/comparison.csv`, `results/g2/validation.json`,
`runs/g2/w1_nt*_seed0_feeder_ext_*`, and `runs/g2/clean_replay/`.

## 5. Validation boundary

`scripts/validate_run.py` re-reads the saved config, events, reports, truth,
output, reference, metadata and timing. It does not control the running model.
The negative control `logs/g2_negative_sequence.log` duplicates an event_seq
and is correctly rejected.

No G2 result is a hardware measurement. The event trace is a model-side,
passive observation designed for later gates.
