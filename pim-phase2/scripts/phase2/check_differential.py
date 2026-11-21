#!/usr/bin/env python3
"""Independent Python replay of every exported L3 decision."""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from phase2.engine import (  # noqa: E402
    CheckpointView,
    DataRef,
    EngineConfig,
    Gate,
    HostRead,
    Ledger,
    OpEntry,
    POLICY_B1,
    POLICY_B2,
    POLICY_B2R,
    UNSTARTED,
    WINDOW_AUTO,
    analyze,
    plan_whole_job,
)
from phase2.layout import Layout, PublicReport  # noqa: E402


def read_params(path: Path) -> dict:
    values = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip()
    return values


def parse_refs(text: str):
    refs = []
    if not text:
        return refs
    for part in text.split(";"):
        if not part:
            continue
        operand, tile, version = part.split(":")
        refs.append(DataRef(int(operand), int(tile), int(version)))
    return refs


def load_ledger(run_dir: Path, truncated: bool,
                host_reads: list[HostRead] | None = None) -> Ledger:
    layout_rows = []
    with (run_dir / "layout.csv").open(newline="") as handle:
        for row in csv.DictReader(handle):
            layout_rows.append((int(row["operand"]), int(row["base_row"])))
    layout = Layout("exported", tuple(sorted(layout_rows)), -1)
    entries = []
    with (run_dir / "ledger.csv").open(newline="") as handle:
        for row in csv.DictReader(handle):
            entries.append(OpEntry(
                op_id=int(row["op_id"]),
                job_id=int(row["job_id"]),
                kernel_idx=int(row["kernel_idx"]),
                tile=int(row["tile"]),
                inputs=parse_refs(row["inputs"]),
                outputs=parse_refs(row["outputs"]),
                t_issue=int(row["t_issue"]),
                t_complete=int(row["t_complete"]),
                completed=row["completed"].strip().lower() in ("1", "true"),
                dep_complete=row["dep_complete"].strip().lower() in ("1", "true"),
                is_replay=row["is_replay"].strip().lower() in ("1", "true"),
            ))
    return Ledger.from_entries(layout, entries, truncated=truncated,
                               host_reads=host_reads or ())


def load_report(run_dir: Path) -> PublicReport:
    with (run_dir / "reports.csv").open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise ValueError("run has no delivered report")
    row = rows[0]
    return PublicReport(int(row["report_seq"]), int(row["report_cycle"]),
                        row["error_class"], row["region_type"],
                        int(row["region_id"]))


def check_run(run_dir: Path) -> tuple[bool, str]:
    params = read_params(run_dir / "params.ini")
    inputs = json.loads((run_dir / "decision_inputs.json").read_text())
    host_reads = [
        HostRead(int(row["operand"]), int(row["tile"]), int(row["version"]),
                 int(row["t_issue"]), int(row["t_complete"]))
        for row in inputs.get("host_reads", [])
    ]
    ledger = load_ledger(run_dir, bool(inputs.get("ledger_truncated", False)),
                         host_reads)
    checkpoint = CheckpointView()
    for operand, tile in inputs.get("checkpoint_missing", []):
        checkpoint.setAbsent(int(operand), int(tile))
    gate = Gate()
    for operand, tile, version in inputs.get("gate_consumed", []):
        ref = DataRef(int(operand), int(tile), int(version))
        gate.publish(ref, 0)
        gate.consume(ref, 0)

    policy = params["policy"]
    if policy == "B0":
        decision = plan_whole_job(ledger, checkpoint=checkpoint)
    else:
        if policy == "B1":
            policy_name, radius = POLICY_B1, 0
        elif policy == "B2":
            policy_name, radius = POLICY_B2, 0
        elif policy == "B2R":
            policy_name, radius = POLICY_B2R, int(params.get("radius_rows", "0"))
        else:
            raise ValueError(f"unknown policy {policy}")
        config = EngineConfig(
            policy=policy_name, radius_rows=radius,
            delta_cycles=int(inputs["delta_cycles"]),
            max_log_entries=int(params.get("max_log_entries", "1000000")),
            window_mode=inputs.get("window_mode", WINDOW_AUTO),
            c_fix_by_kernel=tuple(float(v) for v in inputs.get("kernel_c_fix", [])),
            c_tile_by_kernel=tuple(float(v) for v in inputs.get("kernel_c_tile", [])))
        decision = analyze(ledger, load_report(run_dir), int(inputs["t_ack"]), config,
                           gate=gate, checkpoint=checkpoint,
                           report_overflow=bool(inputs.get("report_overflow", False)))
    retyped = decision.to_json()
    stored = (run_dir / "decision.json").read_text().strip()
    if retyped != stored:
        return False, f"decision mismatch\npython={retyped}\nstored={stored}"
    return True, ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", default=str(ROOT / "runs/phase2/l3"))
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()
    run_root = Path(args.runs)
    run_dirs = sorted(path.parent for path in run_root.glob("*/decision.json"))
    if args.limit:
        run_dirs = run_dirs[:args.limit]
    passed = 0
    failures = []
    for run_dir in run_dirs:
        try:
            ok, detail = check_run(run_dir)
        except Exception as error:  # noqa: BLE001
            ok, detail = False, str(error)
        if ok:
            passed += 1
        else:
            failures.append((run_dir.name, detail))
            print(f"FAIL {run_dir.name}: {detail}")
    print(f"DIFFERENTIAL={passed}/{len(run_dirs)}")
    print(f"DIFFERENTIAL_STATUS={'PASS' if not failures else 'FAIL'}")
    return 0 if run_dirs and not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
