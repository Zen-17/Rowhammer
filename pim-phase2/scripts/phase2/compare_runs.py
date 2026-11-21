#!/usr/bin/env python3
"""Compare the Phase 1 regression before/after Phase 2 hooks (zero-drift gate)."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IGNORED_KEYS = {
    "wall_seconds", "start_time_utc", "generated_utc",
    "paired_ext_off_wall_seconds", "wall_overhead_fraction",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def filtered_metrics(path: Path) -> dict:
    payload = json.loads(path.read_text())
    return {key: value for key, value in payload.items() if key not in IGNORED_KEYS}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", default=str(ROOT / "runs/phase2/regress_before"))
    parser.add_argument("--after", default=str(ROOT / "runs/phase2/regress_after"))
    parser.add_argument("--out", default=str(ROOT / "results/phase2/regress_after/compare.json"))
    args = parser.parse_args()
    before = Path(args.before)
    after = Path(args.after)
    run_dirs = sorted(path.parent for path in before.glob("*/metrics.json"))
    failures = []
    checked = 0
    for before_dir in run_dirs:
        run_id = before_dir.name
        after_dir = after / run_id
        checked += 1
        if not after_dir.exists():
            failures.append({"run_id": run_id, "reason": "missing after run"})
            continue
        before_meta = json.loads((before_dir / "metadata.json").read_text())
        after_meta = json.loads((after_dir / "metadata.json").read_text())
        if before_meta.get("run_status") != after_meta.get("run_status"):
            failures.append({"run_id": run_id, "reason": "run_status drift",
                             "before": before_meta.get("run_status"),
                             "after": after_meta.get("run_status")})
        if filtered_metrics(before_dir / "metrics.json") != filtered_metrics(
                after_dir / "metrics.json"):
            failures.append({"run_id": run_id, "reason": "metrics drift"})
        for name in ("output.bin", "reports.csv"):
            before_file = before_dir / name
            after_file = after_dir / name
            if before_file.exists() != after_file.exists():
                failures.append({"run_id": run_id, "reason": f"{name} presence"})
                continue
            if before_file.exists() and sha256(before_file) != sha256(after_file):
                failures.append({"run_id": run_id, "reason": f"{name} sha256"})
    for after_dir in after.glob("*/metrics.json"):
        if not (before / after_dir.parent.name).exists():
            failures.append({"run_id": after_dir.parent.name, "reason": "extra after run"})

    payload = {
        "before": str(before.relative_to(ROOT)),
        "after": str(after.relative_to(ROOT)),
        "cases_checked": checked,
        "failures": failures,
        "pass": not failures,
        "ignored_keys": sorted(IGNORED_KEYS),
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(f"CASES_CHECKED={checked}")
    for failure in failures[:20]:
        print(f"DRIFT {failure}")
    print(f"REGRESSION={'PASS' if not failures else 'FAIL'}")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
