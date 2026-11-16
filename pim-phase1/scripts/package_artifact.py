#!/usr/bin/env python3
"""Create a reproducible G9 artifact and a non-recursive SHA256 manifest."""
from __future__ import annotations

import hashlib
import json
import os
import tarfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARTIFACT = ROOT / "artifact"
ARCHIVE = ARTIFACT / "phase1-artifact.tar.gz"
MANIFEST = ARTIFACT / "MANIFEST.sha256"
ARCHIVE_HASH = ARTIFACT / "ARCHIVE.sha256"
REPRO = ARTIFACT / "reproduction_log.txt"
EXCLUDE_DIRS = {
    ".venv", ".pytest_cache", "__pycache__", "artifact",
    "simulator/.git", "simulator/bin", "simulator/lib", "simulator/dump",
    "configs/generated/paper", "runs/g8/manual",
}
EXCLUDE_SUFFIXES = {".pyc", ".pyo", ".o", ".d", ".so", ".a", ".tar.gz"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def included(path: Path) -> bool:
    rel = path.relative_to(ROOT).as_posix()
    if rel == "artifact/reproduction_log.txt":
        return path.is_file()
    if rel in {
        "logs/g9_package.log", "logs/g9_final_audit.log", "logs/g9_command_index.md",
        "logs/g9_lock_verification.log", "results/g9_validation.json",
        "environment/g9.lock.json",
    }:
        return False
    if rel == "simulator/sim":
        return False
    for excluded in EXCLUDE_DIRS:
        if rel == excluded or rel.startswith(excluded + "/"):
            return False
    if path.suffix in EXCLUDE_SUFFIXES:
        return False
    if path.name in {".DS_Store"}:
        return False
    return path.is_file()


def main() -> int:
    ARTIFACT.mkdir(parents=True, exist_ok=True)
    for stale in (ARCHIVE, MANIFEST, ARCHIVE_HASH):
        if stale.exists():
            stale.unlink()
    environment = json.loads((ROOT / "environment/environment.json").read_text())
    g8 = json.loads((ROOT / "environment/g8.lock.json").read_text())
    REPRO.write_text(
        "HBM-PIM Phase 1 artifact reproduction log\n"
        f"generated_utc={datetime.now(timezone.utc).isoformat(timespec='seconds')}\n"
        f"upstream_commit={g8['upstream_commit']}\n"
        f"upstream_tree={g8['upstream_tree']}\n"
        f"g8_binary={g8['g8_binary']['sha256']}\n"
        f"g8_patch={g8['source_patch']['sha256']}\n"
        f"environment_hash={g8['environment_hash']}\n"
        f"python={environment.get('python', 'see environment/environment.txt')}\n"
        "\n"
        "Smoke replay:\n"
        "  bash scripts/reproduce.sh --smoke\n"
        "\n"
        "Paper replay:\n"
        "  bash scripts/reproduce.sh --paper\n"
        "\n"
        "Build:\n"
        "  cd simulator && scons NO_STORAGE=0 NO_LIBRARY=1\n"
        "\n"
        "G8 paper matrix:\n"
        "  .venv/bin/python scripts/calibrate_g8.py\n"
        "  .venv/bin/python scripts/expand_suite.py --suite configs/suite_paper.yaml\n"
        "  .venv/bin/python scripts/run_suite.py --manifest configs/generated/paper-manifest.json --jobs 8\n"
        "  .venv/bin/python scripts/aggregate.py --runs runs/paper --out results\n"
        "  .venv/bin/python scripts/plot_paper.py --results results --out figures\n"
        "\n"
        "The artifact excludes .venv, simulator/.git, simulator/bin, simulator/lib,\n"
        "per-case generated config duplicates, and previous archives. The SHA256\n"
        "manifest does not include itself, ARCHIVE.sha256, or the archive.\n"
        "README.md is included in the archive but omitted from MANIFEST.sha256\n"
        "because the external README carries the archive hash.\n"
    )

    files = sorted(path for path in ROOT.rglob("*") if included(path))
    manifest_lines = []
    for path in files:
        if path.relative_to(ROOT).as_posix() == "README.md":
            continue
        manifest_lines.append(f"{sha256(path)}  {path.relative_to(ROOT).as_posix()}")
    MANIFEST.write_text("\n".join(manifest_lines) + "\n")

    with tarfile.open(ARCHIVE, "w:gz", compresslevel=6) as tar:
        for path in files:
            tar.add(path, arcname=f"pim-phase1/{path.relative_to(ROOT).as_posix()}")
        tar.add(MANIFEST, arcname="pim-phase1/artifact/MANIFEST.sha256")
    archive_sha = sha256(ARCHIVE)
    ARCHIVE_HASH.write_text(f"{archive_sha}  artifact/phase1-artifact.tar.gz\n")
    print(f"files={len(files)} manifest={MANIFEST.relative_to(ROOT)}")
    print(f"archive={ARCHIVE.relative_to(ROOT)} sha256={archive_sha}")
    print(f"archive_bytes={ARCHIVE.stat().st_size}")
    print("G9_PACKAGE=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
