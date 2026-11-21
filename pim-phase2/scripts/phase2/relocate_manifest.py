#!/usr/bin/env python3
"""Relocate the frozen Phase 1 regression manifest for this checkout.

Only path-valued fields are rewritten:

  * ``params.device_ini`` / ``params.system_ini`` get the current repository
    prefix;
  * ``params.output_dir`` / ``case.out`` are redirected below
    ``runs/phase2/regress_before`` (or the caller supplied run root).

``run_id`` and every scientific parameter are copied unchanged.  The source
manifest is never modified.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
P1_ROOT = ROOT.parent / "pim-phase1"
DEFAULT_SOURCE = P1_ROOT / "configs/generated/regression_g13-manifest.json"
DEFAULT_OUT = ROOT / "configs/generated/phase2/regression-manifest.json"
DEFAULT_RUNS = ROOT / "runs/phase2/regress_before"


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def infer_old_root(manifest: dict) -> str:
    """Infer the repository prefix baked into the source manifest."""
    candidates = []
    for case in manifest.get("cases", []):
        for key in ("device_ini", "system_ini"):
            value = case.get("params", {}).get(key)
            if isinstance(value, str):
                marker = "/simulator/ini/"
                if marker in value:
                    candidates.append(value.split(marker, 1)[0])
    if not candidates:
        raise SystemExit("cannot infer the old repository prefix from the manifest")
    counts = Counter(candidates)
    return counts.most_common(1)[0][0]


def relocate_text(value: str, old_root: str, new_root: str) -> str:
    if not isinstance(value, str):
        return value
    if old_root and value.startswith(old_root):
        return new_root + value[len(old_root):]
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", default=str(DEFAULT_SOURCE))
    parser.add_argument("--out-manifest", default=str(DEFAULT_OUT))
    parser.add_argument("--run-root", default=str(DEFAULT_RUNS))
    parser.add_argument("--old-root", default="")
    args = parser.parse_args()

    source = Path(args.source).resolve()
    out_manifest = Path(args.out_manifest).resolve()
    run_root = Path(args.run_root).resolve()
    old_root = args.old_root or infer_old_root(json.loads(source.read_text()))

    manifest = json.loads(source.read_text())
    cases = [copy.deepcopy(case) for case in manifest["cases"]]
    for case in cases:
        params = case.setdefault("params", {})
        for key in ("device_ini", "system_ini"):
            if key in params:
                params[key] = relocate_text(params[key], old_root, str(P1_ROOT))
        run_id = case["run_id"]
        out = run_root / run_id
        params["output_dir"] = str(out)
        case["case"]["out"] = str(out)

    counts = Counter(case["group"] for case in cases)
    relocated = copy.deepcopy(manifest)
    relocated["suite"] = "phase2_regression"
    relocated["generated_utc"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    relocated["source_manifest"] = (str(source.relative_to(ROOT.parent))
                                    if source.is_relative_to(ROOT.parent) else str(source))
    relocated["source_manifest_sha256"] = sha256_file(source)
    relocated["phase1_root"] = str(P1_ROOT)
    relocated["phase2_root"] = str(ROOT)
    relocated["relocated_from"] = old_root
    relocated["run_root"] = str(run_root.relative_to(ROOT)) if run_root.is_relative_to(ROOT) else str(run_root)
    relocated["expected_total"] = len(cases)
    relocated["expected_zero_drift"] = True
    relocated["cases"] = cases

    out_manifest.parent.mkdir(parents=True, exist_ok=True)
    out_manifest.write_text(json.dumps(relocated, indent=2) + "\n")
    print(f"RELOCATED_MANIFEST={out_manifest.relative_to(ROOT)}")
    print(f"RUN_ROOT={relocated['run_root']}")
    print(f"OLD_ROOT={old_root}")
    print(f"PHASE1_ROOT={P1_ROOT}")
    print(f"PHASE2_ROOT={ROOT}")
    print(f"CASES={len(cases)}")
    for group, count in sorted(counts.items()):
        print(f"  {group}: {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
