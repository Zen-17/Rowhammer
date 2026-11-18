#!/usr/bin/env python3
"""Build the G10 regression manifest from the frozen paper manifest.

The selected cases cover one deterministic subset of every paper group.  Only
the output locations are redirected; every scientific parameter is copied from
configs/generated/paper-manifest.json unchanged.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAPER_MANIFEST = ROOT / "configs/generated/paper-manifest.json"
OUT_MANIFEST = ROOT / "configs/generated/regression_g10-manifest.json"
REGRESSION_RUNS = ROOT / "runs/regression_g10"

EXPECTED_BY_GROUP = {
    "P1-A": 144,
    "P1-C": 60,
    "P1-E": 2,
    "P1-F Cost": 16,
    "P1-F Robustness": 18,
}
EXPECTED_TOTAL = 240


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def selected(case: dict) -> bool:
    group = case["group"]
    meta = case["case"]
    if group == "P1-A":
        return True
    if group == "P1-C":
        return (meta.get("seed") == 0
                and case["params"].get("profile") == "LOCAL_VISIBLE")
    if group == "P1-E":
        return meta.get("kind") == "positive" and meta.get("seed") == 0
    if group == "P1-F Cost":
        return meta.get("seed") == 0
    if group == "P1-F Robustness":
        return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-manifest", default=str(OUT_MANIFEST))
    parser.add_argument("--run-root", default=str(REGRESSION_RUNS))
    args = parser.parse_args()
    out_manifest = Path(args.out_manifest).resolve()
    run_root = Path(args.run_root).resolve()

    paper = json.loads(PAPER_MANIFEST.read_text())
    cases = [copy.deepcopy(case) for case in paper["cases"] if selected(case)]
    counts = Counter(case["group"] for case in cases)
    if dict(counts) != EXPECTED_BY_GROUP or len(cases) != EXPECTED_TOTAL:
        raise SystemExit(
            f"unexpected regression selection: total={len(cases)} groups={dict(counts)}")

    seen: set[str] = set()
    for case in cases:
        run_id = case["run_id"]
        if run_id in seen:
            raise SystemExit(f"duplicate run_id: {run_id}")
        seen.add(run_id)
        out = run_root / run_id
        case["params"]["output_dir"] = str(out)
        case["case"]["out"] = str(out)

    cases.sort(key=lambda case: (case["group"], case["run_id"]))
    manifest = {
        "schema_version": 1,
        "suite": "regression_g10",
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "source_manifest": str(PAPER_MANIFEST.relative_to(ROOT)),
        "source_manifest_sha256": sha256_file(PAPER_MANIFEST),
        "upstream_commit": paper["upstream_commit"],
        "expected_total": EXPECTED_TOTAL,
        "expected_negative": sum(1 for case in cases if case["expected_negative"]),
        "expected_by_group": EXPECTED_BY_GROUP,
        "selection": {
            "P1-A": "all",
            "P1-C": "LOCAL_VISIBLE seed==0 main matrix",
            "P1-E": "kind==positive seed==0",
            "P1-F Cost": "seed==0",
            "P1-F Robustness": "all",
        },
        "cases": cases,
    }
    out_manifest.parent.mkdir(parents=True, exist_ok=True)
    out_manifest.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {out_manifest.relative_to(ROOT)}")
    print(f"REGRESSION_MANIFEST_TOTAL={len(cases)}")
    for group in EXPECTED_BY_GROUP:
        print(f"  {group}: {counts[group]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
