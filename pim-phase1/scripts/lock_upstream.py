#!/usr/bin/env python3
"""Write environment/upstream.lock.json from the actual fixed worktree."""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SIM_DIR = PROJECT_ROOT / "simulator"
UPSTREAM_COMMIT = "3703d1f19c8f027360cc33a3243eb271e3bb6898"
UPSTREAM_URL = "https://github.com/SAITPublic/PIMSimulator.git"


def sh(*args: str) -> str:
    return subprocess.check_output(args, text=True).strip()


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def file_record(path: Path, root: Path) -> dict:
    return {
        "path": str(path.relative_to(root)),
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
    }


def test_record(xml_rel: str) -> dict | None:
    p = PROJECT_ROOT / xml_rel
    if not p.exists():
        return None
    root = ET.parse(p).getroot()
    return {
        "xml": xml_rel,
        "sha256": sha256(p),
        "tests": int(root.attrib.get("tests", "0")),
        "failures": int(root.attrib.get("failures", "0")),
        "errors": int(root.attrib.get("errors", "0")),
    }


def main() -> int:
    head = sh("git", "-C", str(SIM_DIR), "rev-parse", "HEAD")
    if head != UPSTREAM_COMMIT:
        print(f"ERROR: HEAD {head} != {UPSTREAM_COMMIT}", file=sys.stderr)
        return 2
    status = sh("git", "-C", str(SIM_DIR), "status", "--porcelain")
    if status:
        print("ERROR: upstream worktree is dirty:\n" + status, file=sys.stderr)
        return 3

    configs = [
        file_record(SIM_DIR / rel, SIM_DIR)
        for rel in ("ini/HBM2_samsung_2M_16B_x64.ini", "system_hbm.ini",
                    "system_hbm_1ch.ini", "system_hbm_64ch.ini")
    ]
    licenses = [
        file_record(SIM_DIR / rel, SIM_DIR)
        for rel in ("LICENSE-PIMSimulator", "LICENSE-DRAMSIM2")
    ]

    binary = SIM_DIR / "sim"
    build_logs = sorted((PROJECT_ROOT / "logs").glob("build_*.log"))
    baseline_logs = sorted((PROJECT_ROOT / "logs").glob("baseline_*.log"))
    req = PROJECT_ROOT / "environment/requirements.lock.txt"
    handoff = PROJECT_ROOT / "HANDOFF.md"

    record = {
        "schema_version": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "repository": {
            "url": UPSTREAM_URL,
            "commit": head,
            "git_tree": sh("git", "-C", str(SIM_DIR), "rev-parse", "HEAD^{tree}"),
            "commit_date": sh("git", "-C", str(SIM_DIR), "log", "-1", "--format=%cI"),
            "commit_subject": sh("git", "-C", str(SIM_DIR), "log", "-1", "--format=%s"),
            "ref_checked": "refs/heads/dev",
            "ref_check_log": "logs/g0_ref_check.log",
            "clone_method": "git clone --depth 1 --branch dev, then detach at fixed commit",
            "shallow_clone": sh("git", "-C", str(SIM_DIR), "rev-parse", "--is-shallow-repository") == "true",
            "worktree_clean": True,
        },
        "licenses": licenses,
        "original_configs": configs,
        "build": {
            "command": "scons NO_STORAGE=0 NO_LIBRARY=1",
            "storage_enabled": True,
            "no_storage_argument": 0,
            "no_storage_define_present": False,
            "cxxflags": "-g -O2 -std=c++14 -Wall -Wno-reorder -Wno-sign-compare",
            "binary": file_record(binary, PROJECT_ROOT) if binary.exists() else None,
            "build_flags_file": "environment/build_flags.txt",
            "build_logs": [file_record(p, PROJECT_ROOT) for p in build_logs],
        },
        "tests": {
            "required": test_record("runs/baseline/upstream-tests.xml"),
            "all_original_optional": test_record("runs/baseline/upstream-all-tests.xml"),
            "baseline_logs": [file_record(p, PROJECT_ROOT) for p in baseline_logs],
        },
        "python": {
            "requirements_lock": file_record(req, PROJECT_ROOT) if req.exists() else None,
        },
        "handoff": file_record(handoff, PROJECT_ROOT) if handoff.exists() else None,
    }
    (PROJECT_ROOT / "environment/upstream.lock.json").write_text(
        json.dumps(record, indent=2) + "\n"
    )
    print(f"wrote {PROJECT_ROOT / 'environment/upstream.lock.json'}")
    print(json.dumps({
        "commit": head,
        "binary_sha256": record["build"]["binary"]["sha256"] if record["build"]["binary"] else None,
        "required_tests": record["tests"]["required"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
