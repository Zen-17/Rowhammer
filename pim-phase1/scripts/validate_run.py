#!/usr/bin/env python3
"""Validate a G2 Phase1 run's trace, data protocol and no-fault output."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path

import jsonschema
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
UPSTREAM_COMMIT = "3703d1f19c8f027360cc33a3243eb271e3bb6898"
EVENT_HEADER = [
    "event_seq", "cycle", "phase", "event_type", "kernel_id", "version_id", "access_id",
    "channel", "rank", "actual_bank", "row", "column", "access_path", "bytes", "status",
]
REPORT_HEADER = ["report_seq", "report_cycle", "error_class", "region_type", "region_id"]
TRUTH_HEADER = [
    "record_type", "detection_id", "fault_id", "fault_cycle", "storage_key", "codeword_index",
    "bit_positions", "raw_before", "raw_after", "target_access_id", "detect_cycle",
    "decoder_status", "eligible_report_id", "dropped",
]
PHASE_ORDER = {"ENQUEUE": 0, "START": 1, "FEEDER": 2, "MEMORY": 3, "COMPLETE": 4, "FINALIZE": 5}
NUMERIC_CONFIG = {
    "schema_version", "channels", "ranks", "capacity_mb", "n_tiles", "seed",
    "feeder_outstanding_limit", "feeder_issue_per_cycle", "watchdog_cycles", "dim_bursts",
    "elements",
}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_params(path: Path) -> dict:
    values: dict[str, str] = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line[0] in ";#":
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    config: dict = {}
    for key, value in values.items():
        if key in NUMERIC_CONFIG:
            config[key] = int(value)
        else:
            config[key] = value
    return config


def load_schema() -> dict:
    return json.loads((ROOT / "configs/schema.json").read_text())


def read_csv(path: Path, expected_header: list[str]) -> list[dict]:
    with path.open(newline="") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames != expected_header:
            raise ValueError(f"{path}: header mismatch: {reader.fieldnames} != {expected_header}")
        return list(reader)


def reference(config: dict):
    if config["workload"] == "W1":
        channels = config["channels"]
        ranks = config["ranks"]
        dim = config["n_tiles"] * 16 * channels * ranks * 8
        elements = dim * 16
        index = np.arange(elements, dtype=np.int64)
        seed = config["seed"]
        a = (((index + seed) % 17 - 8).astype(np.float32) / np.float32(8.0)).astype(np.float16)
        x = (((3 * index + seed) % 13 - 6).astype(np.float32) / np.float32(8.0)).astype(np.float16)
        b = (a + x).astype(np.float16)
        return a, x, b, dim, elements
    if config["workload"] == "W0":
        values = ((np.arange(16, dtype=np.float32) + 1) / np.float32(8.0)).astype(np.float16)
        return None, None, values, 1, 16
    return None, None, None, 0, 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--binary", default=str(ROOT / "simulator/sim"))
    parser.add_argument("--source-patch", default=str(ROOT / "environment/source.patch"))
    args = parser.parse_args()
    run = Path(args.run_dir).resolve()
    params_path = run / "params.ini"
    schema = load_schema()
    assertions = []

    def check(name, passed, expected, actual, event_seq=None):
        assertions.append({
            "name": name,
            "pass": bool(passed),
            "expected": expected,
            "actual": actual,
            "event_seq": event_seq,
        })
        return bool(passed)

    try:
        config = parse_params(params_path)
        jsonschema.validate(instance=config, schema=schema)
        check("config_schema", True, "valid", "valid")
    except Exception as exc:  # noqa: BLE001 - reported as an assertion
        check("config_schema", False, "valid", str(exc))
        config = {}

    events = []
    event_error = None
    try:
        events = read_csv(run / "events.csv", EVENT_HEADER)
        for row in events:
            schema_row = dict(row)
            schema_row["event_seq"] = int(row["event_seq"])
            schema_row["cycle"] = int(row["cycle"])
            jsonschema.validate(instance=schema_row, schema=schema["definitions"]["event_row"])
        check("events_schema", True, "valid rows", len(events))
    except Exception as exc:  # noqa: BLE001
        event_error = str(exc)
        check("events_schema", False, "valid rows", event_error)

    if events:
        last_seq = 0
        last_cycle = -1
        last_phase = -1
        sequence_ok = True
        cycle_ok = True
        phase_ok = True
        location_ok = True
        for row in events:
            seq = int(row["event_seq"])
            cycle = int(row["cycle"])
            phase = PHASE_ORDER.get(row["phase"], -1)
            if seq != last_seq + 1:
                sequence_ok = False
            if cycle < last_cycle or (cycle == last_cycle and phase < last_phase):
                cycle_ok = False
                phase_ok = False
            if cycle > last_cycle:
                last_cycle = cycle
                last_phase = phase
            else:
                last_phase = max(last_phase, phase)
            last_seq = seq
            if row["event_type"] in {"DESCRIPTOR_ENQUEUE", "CONTROLLER_ACCEPT", "CONTROLLER_REJECT",
                                     "READ_COMPLETE", "WRITE_DATA_DONE"}:
                try:
                    channel = int(row["channel"])
                    rank = int(row["rank"])
                    if not (0 <= channel < config["channels"] and 0 <= rank < config["ranks"]):
                        location_ok = False
                    access_path = row["access_path"]
                    if row["actual_bank"] == "":
                        if "PIM_BROADCAST" not in access_path and "PIM_CONTROL" not in access_path:
                            location_ok = False
                    else:
                        bank = int(row["actual_bank"])
                        if not (0 <= bank < 16):
                            location_ok = False
                    if row["row"] == "" or row["column"] == "":
                        if "PIM_CONTROL" not in access_path:
                            location_ok = False
                    else:
                        row_index = int(row["row"])
                        column = int(row["column"])
                        if not (0 <= row_index < 16384 and 0 <= column < 32):
                            location_ok = False
                except (ValueError, KeyError):
                    location_ok = False
        check("event_seq_strict", sequence_ok, "1..N contiguous", len(events))
        check("event_cycle_monotonic", cycle_ok, "nondecreasing", last_cycle)
        check("event_phase_order", phase_ok, "per-cycle phase nondecreasing", "checked")
        check("event_location_range", location_ok, "channel/rank/bank/row/column in range", "checked")
    else:
        check("event_seq_strict", True, "empty allowed", 0)
        check("event_cycle_monotonic", True, "empty allowed", 0)
        check("event_phase_order", True, "empty allowed", 0)
        check("event_location_range", True, "empty allowed", 0)

    trace_mode = config.get("trace_mode", "ext_off")
    if trace_mode == "ext_on":
        check("events_present", len(events) > 0, ">0", len(events))
        types = [row["event_type"] for row in events]
        check("events_have_enqueue_accept_complete",
              types.count("DESCRIPTOR_ENQUEUE") > 0 and types.count("CONTROLLER_ACCEPT") > 0
              and (types.count("READ_COMPLETE") + types.count("WRITE_DATA_DONE")) > 0,
              "enqueue/accept/complete", {"enqueue": types.count("DESCRIPTOR_ENQUEUE"),
                                          "accept": types.count("CONTROLLER_ACCEPT"),
                                          "complete": types.count("READ_COMPLETE") + types.count("WRITE_DATA_DONE")})
        check("events_accept_matches_enqueue",
              types.count("CONTROLLER_ACCEPT") == types.count("DESCRIPTOR_ENQUEUE"),
              types.count("DESCRIPTOR_ENQUEUE"), types.count("CONTROLLER_ACCEPT"))
        check("events_complete_matches_accept",
              (types.count("READ_COMPLETE") + types.count("WRITE_DATA_DONE"))
              == types.count("CONTROLLER_ACCEPT"),
              types.count("CONTROLLER_ACCEPT"),
              types.count("READ_COMPLETE") + types.count("WRITE_DATA_DONE"))
        check("events_run_brackets", types.count("RUN_START") == 1 and types.count("RUN_END") == 1,
              {"start": 1, "end": 1}, {"start": types.count("RUN_START"), "end": types.count("RUN_END")})
    else:
        check("events_empty_when_ext_off", len(events) == 0, 0, len(events))

    try:
        reports = read_csv(run / "reports.csv", REPORT_HEADER)
        check("reports_header_and_empty", len(reports) == 0, 0, len(reports))
    except Exception as exc:  # noqa: BLE001
        check("reports_header_and_empty", False, 0, str(exc))
    try:
        truth = read_csv(run / "truth.csv", TRUTH_HEADER)
        check("truth_header_and_empty", len(truth) == 0, 0, len(truth))
    except Exception as exc:  # noqa: BLE001
        check("truth_header_and_empty", False, 0, str(exc))

    timing = json.loads((run / "timing.json").read_text())
    check("timing_no_timeout", timing.get("timeout") is False, False, timing.get("timeout"))
    check("timing_queues_empty", timing.get("pending_at_end") == 0
          and timing.get("outstanding_at_end") == 0, {"pending": 0, "outstanding": 0},
          {"pending": timing.get("pending_at_end"), "outstanding": timing.get("outstanding_at_end")})
    check("timing_mode", timing.get("trace_mode") == trace_mode, trace_mode,
          timing.get("trace_mode"))

    a_ref, x_ref, b_ref, dim, elements = reference(config)
    check("element_count", config.get("elements") == elements, elements, config.get("elements"))
    check("burst_count", config.get("dim_bursts") == dim, dim, config.get("dim_bursts"))
    bit_exact = False
    input_ok = True
    if b_ref is not None:
        output = np.frombuffer((run / "output.bin").read_bytes(), dtype="<u2")
        reference_path = run / "reference.bin"
        reference_path.write_bytes(b_ref.view("<u2").tobytes())
        bit_exact = output.size == b_ref.size and np.array_equal(
            output.view(np.uint16), b_ref.view(np.uint16))
        check("output_bit_exact", bit_exact, "bit-identical", {"output_words": output.size,
                                                                "reference_words": b_ref.size})
        check("output_size", len((run / "output.bin").read_bytes()) == elements * 2,
              elements * 2, len((run / "output.bin").read_bytes()))
        if a_ref is not None:
            a_bytes = (run / "input_a.bin").read_bytes()
            x_bytes = (run / "input_x.bin").read_bytes()
            input_ok = (a_bytes == a_ref.view("<u2").tobytes()
                        and x_bytes == x_ref.view("<u2").tobytes())
            check("input_bit_exact", input_ok, "bit-identical", "checked")
    else:
        check("output_bit_exact", False, "unsupported workload", config.get("workload"))

    metadata_path = run / "metadata.json"
    metadata = json.loads(metadata_path.read_text()) if metadata_path.exists() else {}
    required_metadata = {"schema_version", "run_id", "experiment", "seed", "upstream_commit",
                         "patch_sha256", "binary_sha256", "config_sha256", "reference_id",
                         "environment_hash", "start_time_utc", "wall_seconds", "exit_code",
                         "run_status"}
    metadata_ok = required_metadata.issubset(metadata)
    check("metadata_fields", metadata_ok, sorted(required_metadata),
          sorted(required_metadata.intersection(metadata)))
    if metadata_ok:
        check("metadata_config_hash", metadata["config_sha256"] == sha256(params_path),
              sha256(params_path), metadata["config_sha256"])
        check("metadata_upstream", metadata["upstream_commit"] == UPSTREAM_COMMIT,
              UPSTREAM_COMMIT, metadata["upstream_commit"])
        check("metadata_binary", metadata["binary_sha256"] == sha256(Path(args.binary)),
              sha256(Path(args.binary)), metadata["binary_sha256"])
        check("metadata_patch", metadata["patch_sha256"] == sha256(Path(args.source_patch)),
              sha256(Path(args.source_patch)), metadata["patch_sha256"])
        env_hash = json.loads((ROOT / "environment/environment.json").read_text())["environment_hash"]
        check("metadata_environment", metadata["environment_hash"] == env_hash, env_hash,
              metadata["environment_hash"])

    passed = all(item["pass"] for item in assertions)
    outcomes = {
        "contract_pass": passed,
        "outcome_class": None,
        "output_changed": not bit_exact,
        "invalid_outputs": 0 if bit_exact else 1,
        "event_count": len(events),
        "producer_cycles": timing.get("cycles"),
        "end_to_end_cycles": timing.get("cycles"),
        "timeout": timing.get("timeout"),
    }
    metrics = {
        "contract_pass": passed,
        "outcome_class": None,
        "output_changed": not bit_exact,
        "changed_elements": 0 if bit_exact else None,
        "invalid_outputs": 0 if bit_exact else 1,
        "detected_events": None,
        "delivered_reports": None,
        "dropped_reports": None,
        "report_latency_cycles": None,
        "quiesce_cycles": None,
        "restore_cycles": None,
        "reexec_cycles": None,
        "producer_cycles": timing.get("cycles"),
        "end_to_end_cycles": timing.get("cycles"),
        "host_transfer_bytes": None,
        "restore_write_bytes": None,
        "peak_held_output_bytes": None,
        "checkpoint_bytes": None,
        "parity_bytes": None,
        "runtime_metadata_bytes": None,
        "timeout": timing.get("timeout"),
        "event_count": len(events),
    }
    validation = {
        "run_status": "PASS" if passed else "FAIL",
        "contract_pass": passed,
        "assertions": assertions,
    }
    (run / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    (run / "validation.json").write_text(json.dumps(validation, indent=2) + "\n")
    if metadata_ok:
        metadata["run_status"] = "PASS" if passed else "FAIL"
        metadata["output_sha256"] = sha256(run / "output.bin")
        metadata["reference_sha256"] = sha256(run / "reference.bin")
        metadata["event_count"] = len(events)
        metadata["trace_mode"] = trace_mode
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    for item in assertions:
        print(("PASS" if item["pass"] else "FAIL"), item["name"], item["expected"], item["actual"])
    print("G2_RUN_VALIDATION=" + ("PASS" if passed else "FAIL"))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
