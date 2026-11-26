#!/usr/bin/env python3
"""M4.2 SRAM Hardware Cost Calculator.

Computes exact bit widths and byte counts per pseudo-channel and outputs
phase3_exp/results/E7_hardware_cost.csv along with a JSON summary to stdout.
"""
from __future__ import annotations

import csv
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RESULTS_DIR = EXP = ROOT / "phase3_exp" / "results"


def compute_sram_cost() -> dict:
    components = [
        {
            "component": "region_map_table",
            "scope": "per pseudo-channel",
            "entries": 256,
            "bits_per_entry": 95,
            "total_bits": 256 * 95,
            "total_bytes": (256 * 95) // 8,
            "formula": "256 * (6(key_old) + 6(key_new) + 6(next_pair) + 2(state) + 2(quiet_cnt) + 1(alarmed) + 8(epoch_tag) + 64(migrated_bitmap))",
            "notes": "Full 95-bit variant with migrated bitmap. Pointer-only variant (without bitmap): 31 bits/region = 7936 bits = 992 bytes."
        },
        {
            "component": "misra_gries_detector",
            "scope": "per pseudo-channel",
            "entries": 16 * 15,
            "bits_per_entry": 24,
            "total_bits": 16 * 15 * 24,
            "total_bytes": (16 * 15 * 24) // 8,
            "formula": "16 * 15 * (14(row) + 10(count))",
            "notes": "16 banks x 15 Misra-Gries slots/bank."
        },
        {
            "component": "l2_soft_repair_cam",
            "scope": "per pseudo-channel",
            "entries": 16 * 16,
            "bits_per_entry": 19,
            "total_bits": 16 * 16 * 19,
            "total_bytes": (16 * 16 * 19) // 8,
            "formula": "16 * 16 * (14(source_row) + 4(spare_idx) + 1(valid))",
            "notes": "16 banks x 16 L2 spare entries/bank."
        },
        {
            "component": "compute_epoch_registers",
            "scope": "per pseudo-channel",
            "entries": 64 + 1 + 256,
            "bits_per_entry": 8,
            "total_bits": (64 * 8) + (1 * 8) + (256 * 8),
            "total_bytes": ((64 * 8) + (1 * 8) + (256 * 8)) // 8,
            "formula": "(64 report queue + 1 active channel + 256 op ledger) * 8-bit epoch",
            "notes": "Compute-side epoch state including report queue, active channel register, and Phase 2 op ledger."
        }
    ]

    total_bits = sum(c["total_bits"] for c in components)
    total_bytes = sum(c["total_bytes"] for c in components)
    total_kib = total_bytes / 1024.0

    total_row = {
        "component": "TOTAL",
        "scope": "per pseudo-channel",
        "entries": sum(c["entries"] for c in components),
        "bits_per_entry": "",
        "total_bits": total_bits,
        "total_bytes": total_bytes,
        "formula": "Sum of all components",
        "notes": f"Total: {total_bits} bits = {total_bytes} bytes = {total_kib:.2f} KiB per pseudo-channel, {total_bytes * 16 / 1024:.1f} KB across 16 pseudo-channels."
    }

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    csv_path = RESULTS_DIR / "E7_hardware_cost.csv"
    fields = ["component", "scope", "entries", "bits_per_entry", "total_bits", "total_bytes", "formula", "notes"]

    with csv_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for c in components:
            writer.writerow(c)
        writer.writerow(total_row)

    summary = {
        "components": components,
        "total_bits": total_bits,
        "total_bytes": total_bytes,
        "total_kib": round(total_kib, 4),
        "total_kb_16ch": round(total_bytes * 16 / 1024.0, 2),
        "csv_path": str(csv_path)
    }
    return summary


def main() -> None:
    summary = compute_sram_cost()
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
