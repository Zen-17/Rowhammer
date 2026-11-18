#!/usr/bin/env python3
"""Compare G10 regression runs with the frozen paper runs.

metrics.json is compared key by key after removing wall-clock and UTC keys.
output.bin and reports.csv are compared by SHA256, and both sides must agree on
whether the file exists.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGRESSION_MANIFEST = ROOT / "configs/generated/regression_g10-manifest.json"
PAPER_MANIFEST = ROOT / "configs/generated/paper-manifest.json"
OUT_DIR = ROOT / "results/regression_g10"
OUT_CSV = OUT_DIR / "compare.csv"
OUT_SUMMARY = OUT_DIR / "compare_summary.txt"

IGNORED_METRIC_KEYS = {
    "wall_seconds",
    "paired_ext_off_wall_seconds",
    "wall_overhead_fraction",
}
FILE_FIELDS = ("output.bin", "reports.csv")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def scalar_text(value: object) -> str:
    if isinstance(value, (dict, list)):
        return json.dumps(value, sort_keys=True, separators=(",", ":"))
    return json.dumps(value)


def ignored(key: str) -> bool:
    return key in IGNORED_METRIC_KEYS or key.endswith("_utc")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", default=str(REGRESSION_MANIFEST))
    parser.add_argument("--out-dir", default=str(OUT_DIR))
    args = parser.parse_args()
    manifest_path = Path(args.manifest).resolve()
    out_dir = Path(args.out_dir).resolve()
    regression = json.loads(manifest_path.read_text())
    paper = json.loads(PAPER_MANIFEST.read_text())
    paper_out = {case["run_id"]: Path(case["case"]["out"]) for case in paper["cases"]}

    rows: list[dict[str, str]] = []
    for case in regression["cases"]:
        run_id = case["run_id"]
        group = case["group"]
        new_dir = Path(case["case"]["out"])
        if run_id not in paper_out:
            rows.append({
                "run_id": run_id, "group": group, "field": "paper_run",
                "paper_value": "MISSING", "new_value": "PRESENT",
            })
            continue
        old_dir = paper_out[run_id]

        old_metrics_path = old_dir / "metrics.json"
        new_metrics_path = new_dir / "metrics.json"
        if not old_metrics_path.exists() or not new_metrics_path.exists():
            rows.append({
                "run_id": run_id, "group": group, "field": "metrics.json",
                "paper_value": "PRESENT" if old_metrics_path.exists() else "MISSING",
                "new_value": "PRESENT" if new_metrics_path.exists() else "MISSING",
            })
        else:
            old_metrics = json.loads(old_metrics_path.read_text())
            new_metrics = json.loads(new_metrics_path.read_text())
            for key in sorted(set(old_metrics) | set(new_metrics)):
                if ignored(key):
                    continue
                old_present = key in old_metrics
                new_present = key in new_metrics
                if old_present and new_present and old_metrics[key] == new_metrics[key]:
                    continue
                rows.append({
                    "run_id": run_id,
                    "group": group,
                    "field": key,
                    "paper_value": scalar_text(old_metrics[key]) if old_present else "MISSING",
                    "new_value": scalar_text(new_metrics[key]) if new_present else "MISSING",
                })

        for filename in FILE_FIELDS:
            old_path = old_dir / filename
            new_path = new_dir / filename
            old_exists = old_path.exists()
            new_exists = new_path.exists()
            if not old_exists and not new_exists:
                continue
            if old_exists != new_exists:
                rows.append({
                    "run_id": run_id,
                    "group": group,
                    "field": filename,
                    "paper_value": sha256_file(old_path) if old_exists else "MISSING",
                    "new_value": sha256_file(new_path) if new_exists else "MISSING",
                })
                continue
            old_hash = sha256_file(old_path)
            new_hash = sha256_file(new_path)
            if old_hash != new_hash:
                rows.append({
                    "run_id": run_id,
                    "group": group,
                    "field": filename,
                    "paper_value": old_hash,
                    "new_value": new_hash,
                })

    out_dir.mkdir(parents=True, exist_ok=True)
    out_csv = out_dir / "compare.csv"
    out_summary = out_dir / "compare_summary.txt"
    with out_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=["run_id", "group", "field", "paper_value", "new_value"])
        writer.writeheader()
        writer.writerows(rows)

    if rows:
        summary = f"REGRESSION=FAIL n_diff={len(rows)}"
    else:
        summary = "REGRESSION=PASS"
    out_summary.write_text(summary + "\n")
    print(summary)
    print(f"cases={len(regression['cases'])} compare={out_csv.relative_to(ROOT)}")
    return 0 if not rows else 1


if __name__ == "__main__":
    raise SystemExit(main())
