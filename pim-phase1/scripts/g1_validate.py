#!/usr/bin/env python3
"""Independent G1 validation for the Phase1 W0/W1 and address-mapping runs.

The reference is computed here with numpy float16; the C++ output and reference
are compared as raw little-endian 16-bit words, not with a tolerance.
"""
from __future__ import annotations

import csv
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
RUNS = ROOT / "runs/g1"
RESULTS = ROOT / "results/g1"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_params(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line[0] in ";#":
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def ref_w1(n_tiles: int, seed: int, channels: int = 1, ranks: int = 1, banks: int = 16,
           grf: int = 8):
    dim = n_tiles * banks * channels * ranks * grf
    elements = dim * 16
    index = np.arange(elements, dtype=np.int64)
    a = (((index + seed) % 17 - 8).astype(np.float32) / np.float32(8.0)).astype(np.float16)
    x = (((3 * index + seed) % 13 - 6).astype(np.float32) / np.float32(8.0)).astype(np.float16)
    b = (a.astype(np.float16) + x.astype(np.float16)).astype(np.float16)
    return a, x, b


def words(path: Path) -> np.ndarray:
    return np.frombuffer(path.read_bytes(), dtype="<u2")


def bits_equal(path: Path, values: np.ndarray) -> bool:
    data = words(path)
    return data.size == values.size and np.array_equal(data.view(np.uint16), values.view(np.uint16))


def write_comparison(run: Path, record: dict) -> None:
    (run / "comparison.json").write_text(json.dumps(record, indent=2) + "\n")


def validate_w0() -> list[dict]:
    records = {}
    for mode in ("direct", "feeder"):
        run = RUNS / f"w0_1ch_{mode}"
        params = read_params(run / "params.ini")
        timing = json.loads((run / "timing.json").read_text())
        expected = ((np.arange(16, dtype=np.float32) + 1) / np.float32(8.0)).astype(np.float16)
        output = words(run / "output.bin")
        (run / "reference.bin").write_bytes(expected.tobytes())
        ok = output.size == 16 and np.array_equal(output.view(np.uint16), expected.view(np.uint16))
        record = {
            "case_id": f"W0_1ch_{mode}",
            "workload": "W0",
            "mode": mode,
            "channels": int(params["channels"]),
            "n_tiles": 0,
            "seed": 0,
            "dim_bursts": 1,
            "elements": 16,
            "cycles": timing["cycles"],
            "updates": timing["updates"],
            "timeout": timing["timeout"],
            "pending_at_end": timing["pending_at_end"],
            "outstanding_at_end": timing["outstanding_at_end"],
            "max_external_pending": timing["max_external_pending"],
            "max_accepted_outstanding": timing["max_accepted_outstanding"],
            "controller_rejections": timing["controller_rejections"],
            "output_sha256": sha256(run / "output.bin"),
            "reference_sha256": sha256(run / "reference.bin"),
            "bit_exact": bool(ok),
            "config_sha256": sha256(run / "params.ini"),
        }
        write_comparison(run, record)
        records[mode] = record
    equal = (RUNS / "w0_1ch_direct/output.bin").read_bytes() == (RUNS / "w0_1ch_feeder/output.bin").read_bytes()
    for mode in ("direct", "feeder"):
        record = records[mode]
        record["direct_feeder_equal"] = bool(equal)
        record["direct_cycles"] = records["direct"]["cycles"]
        record["feeder_cycles"] = records["feeder"]["cycles"]
        record["cycle_delta_feeder_minus_direct"] = records["feeder"]["cycles"] - records["direct"]["cycles"]
        write_comparison(RUNS / f"w0_1ch_{mode}", record)
    return [records["direct"], records["feeder"]]


def validate_w1() -> list[dict]:
    records: list[dict] = []
    for n_tiles in (1, 8):
        for seed in (0, 1, 2):
            a_ref, x_ref, b_ref = ref_w1(n_tiles, seed)
            for mode in ("direct", "feeder"):
                run = RUNS / f"w1_1ch_nt{n_tiles}_seed{seed}_{mode}"
                params = read_params(run / "params.ini")
                timing = json.loads((run / "timing.json").read_text())
                dim = n_tiles * 16 * 1 * 1 * 8
                assert int(params["channels"]) == 1
                assert int(params["n_tiles"]) == n_tiles
                assert int(params["seed"]) == seed
                assert timing["dim_bursts"] == dim, (timing["dim_bursts"], dim)
                assert timing["elements"] == dim * 16
                (run / "reference.bin").write_bytes(b_ref.tobytes())
                input_a_ok = bits_equal(run / "input_a.bin", a_ref)
                input_x_ok = bits_equal(run / "input_x.bin", x_ref)
                output_ok = bits_equal(run / "output.bin", b_ref)
                record = {
                    "case_id": f"W1_1ch_nt{n_tiles}_seed{seed}_{mode}",
                    "workload": "W1",
                    "mode": mode,
                    "channels": 1,
                    "n_tiles": n_tiles,
                    "seed": seed,
                    "dim_bursts": dim,
                    "elements": dim * 16,
                    "cycles": timing["cycles"],
                    "updates": timing["updates"],
                    "timeout": timing["timeout"],
                    "pending_at_end": timing["pending_at_end"],
                    "outstanding_at_end": timing["outstanding_at_end"],
                    "max_external_pending": timing["max_external_pending"],
                    "max_accepted_outstanding": timing["max_accepted_outstanding"],
                    "controller_rejections": timing["controller_rejections"],
                    "output_sha256": sha256(run / "output.bin"),
                    "reference_sha256": sha256(run / "reference.bin"),
                    "input_a_sha256": sha256(run / "input_a.bin"),
                    "input_x_sha256": sha256(run / "input_x.bin"),
                    "input_a_bit_exact": bool(input_a_ok),
                    "input_x_bit_exact": bool(input_x_ok),
                    "bit_exact": bool(output_ok),
                    "config_sha256": sha256(run / "params.ini"),
                }
                write_comparison(run, record)
                records.append(record)
            direct_run = RUNS / f"w1_1ch_nt{n_tiles}_seed{seed}_direct"
            feeder_run = RUNS / f"w1_1ch_nt{n_tiles}_seed{seed}_feeder"
            equal = (direct_run / "output.bin").read_bytes() == (feeder_run / "output.bin").read_bytes()
            direct_rec = next(r for r in records if r["case_id"] == f"W1_1ch_nt{n_tiles}_seed{seed}_direct")
            feeder_rec = next(r for r in records if r["case_id"] == f"W1_1ch_nt{n_tiles}_seed{seed}_feeder")
            for rec, mode in ((direct_rec, "direct"), (feeder_rec, "feeder")):
                rec["input_a_bit_exact"] = bool(rec["input_a_bit_exact"])
                rec["input_x_bit_exact"] = bool(rec["input_x_bit_exact"])
                rec["direct_feeder_equal"] = bool(equal)
                rec["direct_cycles"] = direct_rec["cycles"]
                rec["feeder_cycles"] = feeder_rec["cycles"]
                rec["cycle_delta_feeder_minus_direct"] = feeder_rec["cycles"] - direct_rec["cycles"]
                write_comparison(RUNS / f"w1_1ch_nt{n_tiles}_seed{seed}_{mode}", rec)
    return records


def encode_address(channels: int, channel: int, bank: int, row: int, col: int) -> int:
    row_bits = 14
    col_bits = 5
    bg_bits = 2
    bank_low_bits = 2
    chan_bits = {1: 0, 2: 1}[channels]
    offset_bits = 5
    bank_low = bank & ((1 << bank_low_bits) - 1)
    bg = bank >> bank_low_bits
    a = 0  # rank = 0
    a = (a << row_bits) | row
    a = (a << col_bits) | col
    a = (a << bg_bits) | bg
    a = (a << bank_low_bits) | bank_low
    a = (a << chan_bits) | (channel & ((1 << chan_bits) - 1))
    a = (a << offset_bits)
    return a


def validate_address() -> list[dict]:
    records = []
    for channels in (1, 2):
        run = RUNS / "address_mapping" / f"{channels}ch"
        summary = json.loads((run / "address_mapping.json").read_text())
        with (run / "address_mapping.csv").open(newline="") as fh:
            rows = list(csv.DictReader(fh))
        addresses = set()
        all_ok = True
        for row in rows:
            channel = int(row["channel"])
            bank = int(row["bank"])
            row_idx = int(row["row"])
            col = int(row["burst_column"])
            address = int(row["address"])
            expected = encode_address(channels, channel, bank, row_idx, col)
            decoded_ok = (
                int(row["decoded_channel"]) == channel
                and int(row["decoded_rank"]) == 0
                and int(row["decoded_bank"]) == bank
                and int(row["decoded_row"]) == row_idx
                and int(row["decoded_burst_column"]) == col
            )
            row_ok = (
                address == expected
                and decoded_ok
                and row["roundtrip_ok"] == "1"
                and row["upstream_decoder_ok"] == "1"
                and row["unique_address"] == "1"
            )
            addresses.add(address)
            all_ok = all_ok and row_ok
        summary_ok = (
            summary["all_pass"] is True
            and summary["channels"] == channels
            and summary["tuples"] == len(rows)
            and summary["unique_addresses"] == len(addresses)
            and summary["roundtrip_pass"] == len(rows)
            and summary["upstream_decoder_pass"] == len(rows)
        )
        record = {
            "case_id": f"ADDRESS_{channels}ch",
            "channels": channels,
            "capacity_mb": summary["capacity_mb"],
            "tuples": len(rows),
            "unique_addresses": len(addresses),
            "all_rows_ok": bool(all_ok),
            "summary_ok": bool(summary_ok),
            "summary": summary,
            "csv_sha256": sha256(run / "address_mapping.csv"),
        }
        all_ok = all_ok and summary_ok
        record["all_pass"] = bool(all_ok)
        (run / "address_validation.json").write_text(json.dumps(record, indent=2) + "\n")
        records.append(record)
    return records


def write_summary(w0: list[dict], w1: list[dict]) -> None:
    RESULTS.mkdir(parents=True, exist_ok=True)
    fields = [
        "case_id", "workload", "mode", "channels", "n_tiles", "seed", "dim_bursts",
        "elements", "cycles", "direct_cycles", "feeder_cycles", "cycle_delta_feeder_minus_direct",
        "bit_exact", "direct_feeder_equal", "timeout", "pending_at_end", "outstanding_at_end",
        "max_external_pending", "max_accepted_outstanding", "controller_rejections",
        "output_sha256", "reference_sha256", "config_sha256",
    ]
    with (RESULTS / "summary.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(w0 + w1)
    with (RESULTS / "w0_summary.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(w0)
    with (RESULTS / "w1_summary.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(w1)


def main() -> int:
    if not RUNS.exists():
        print(f"missing runs directory: {RUNS}", file=sys.stderr)
        return 2
    w0 = validate_w0()
    w1 = validate_w1()
    address = validate_address()
    write_summary(w0, w1)

    checks = {
        "w0_bit_exact": all(r["bit_exact"] for r in w0),
        "w0_direct_feeder_equal": all(r["direct_feeder_equal"] for r in w0),
        "w1_output_bit_exact": all(r["bit_exact"] for r in w1),
        "w1_inputs_bit_exact": all(r["input_a_bit_exact"] and r["input_x_bit_exact"] for r in w1),
        "w1_direct_feeder_equal": all(r["direct_feeder_equal"] for r in w1),
        "w1_no_timeout": all(not r["timeout"] for r in w1),
        "w1_queues_empty": all(r["pending_at_end"] == 0 and r["outstanding_at_end"] == 0 for r in w1),
        "w1_observed_outstanding_bound": all(r["max_accepted_outstanding"] <= 8 for r in w1),
        "address_all_pass": all(r["all_pass"] for r in address),
        "address_unique": all(r["unique_addresses"] == r["tuples"] for r in address),
    }
    result = {
        "gate": "G1",
        "pass": all(checks.values()),
        "checks": checks,
        "w0_cases": len(w0),
        "w1_cases": len(w1),
        "address_cases": len(address),
        "w1_cycles": [{"case_id": r["case_id"], "cycles": r["cycles"]} for r in w1],
        "address": address,
    }
    (RESULTS / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(checks, indent=2))
    print("G1_VALIDATION=" + ("PASS" if result["pass"] else "FAIL"))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
