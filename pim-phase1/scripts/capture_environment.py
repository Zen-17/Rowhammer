#!/usr/bin/env python3
"""Capture the actual G0 host/toolchain environment into versioned files.

This script only reads the host and the fixed upstream worktree.  It does not
modify simulator sources or model behavior.
"""
from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SIM_DIR = PROJECT_ROOT / "simulator"
ENV_DIR = PROJECT_ROOT / "environment"
UPSTREAM_COMMIT = "3703d1f19c8f027360cc33a3243eb271e3bb6898"


def run(args: list[str], cwd: Path | None = None) -> str:
    try:
        return subprocess.check_output(args, cwd=cwd, text=True, stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        return f"UNAVAILABLE: {exc}"


def first_line(text: str) -> str:
    return text.splitlines()[0].strip() if text.strip() else ""


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_os_release() -> dict[str, str]:
    data: dict[str, str] = {}
    for line in Path("/etc/os-release").read_text().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            data[k] = v.strip().strip('"')
    return data


def main() -> int:
    ENV_DIR.mkdir(parents=True, exist_ok=True)
    osr = parse_os_release()
    cpuinfo = Path("/proc/cpuinfo").read_text()
    cpu_model = next((line.split(":", 1)[1].strip() for line in cpuinfo.splitlines()
                      if line.lower().startswith("model name")), "unknown")
    mem_kb = next((int(line.split()[1]) for line in Path("/proc/meminfo").read_text().splitlines()
                   if line.startswith("MemTotal:")), 0)
    disk = shutil.disk_usage(PROJECT_ROOT)

    gcc = first_line(run(["gcc", "--version"]))
    gxx = first_line(run(["g++", "--version"]))
    scons_v = run(["scons", "--version"])
    git_v = first_line(run(["git", "--version"]))
    py_v = first_line(run([sys.executable, "--version"]))
    gtest_v = run(["dpkg-query", "-W", "-f=${Version}", "libgtest-dev"])

    head = run(["git", "-C", str(SIM_DIR), "rev-parse", "HEAD"])
    tree = run(["git", "-C", str(SIM_DIR), "rev-parse", "HEAD^{tree}"])
    commit_date = run(["git", "-C", str(SIM_DIR), "log", "-1", "--format=%cI"])
    commit_subject = run(["git", "-C", str(SIM_DIR), "log", "-1", "--format=%s"])
    shallow = run(["git", "-C", str(SIM_DIR), "rev-parse", "--is-shallow-repository"])
    status = run(["git", "-C", str(SIM_DIR), "status", "--porcelain"])
    if head != UPSTREAM_COMMIT:
        print(f"ERROR: HEAD {head} != fixed commit {UPSTREAM_COMMIT}", file=sys.stderr)
        return 2

    configs = []
    for rel in ("ini/HBM2_samsung_2M_16B_x64.ini", "system_hbm.ini",
                "system_hbm_1ch.ini", "system_hbm_64ch.ini"):
        p = SIM_DIR / rel
        configs.append({"path": rel, "bytes": p.stat().st_size, "sha256": sha256(p)})

    binary = SIM_DIR / "sim"
    packages = run([sys.executable, "-m", "pip", "freeze"]).splitlines()
    build_command = "scons NO_STORAGE=0 NO_LIBRARY=1"
    cxxflags = "-g -O2 -std=c++14 -Wall -Wno-reorder -Wno-sign-compare"

    env_core = {
        "upstream_commit": head,
        "os": osr.get("PRETTY_NAME", ""),
        "kernel": platform.release(),
        "arch": platform.machine(),
        "cpu_model": cpu_model,
        "logical_cpus": os.cpu_count(),
        "memory_bytes": mem_kb * 1024,
        "gcc": gcc,
        "gxx": gxx,
        "scons": scons_v,
        "python": py_v,
        "build_command": build_command,
        "cxxflags": cxxflags,
        "storage_enabled": True,
    }
    env_hash = hashlib.sha256(
        json.dumps(env_core, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()

    record = {
        "schema_version": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "project_root": str(PROJECT_ROOT),
        "upstream": {
            "url": "https://github.com/SAITPublic/PIMSimulator.git",
            "commit": head,
            "tree": tree,
            "commit_date": commit_date,
            "commit_subject": commit_subject,
            "shallow_clone": shallow,
            "worktree_clean": status == "",
            "git_status_porcelain": status,
        },
        "host": {
            "os_pretty": osr.get("PRETTY_NAME", ""),
            "os_id": osr.get("ID", ""),
            "os_version": osr.get("VERSION_ID", ""),
            "kernel": platform.release(),
            "arch": platform.machine(),
            "cpu_model": cpu_model,
            "logical_cpus": os.cpu_count(),
            "memory_bytes": mem_kb * 1024,
            "disk_total_bytes": disk.total,
            "disk_free_bytes": disk.free,
            "python_executable": sys.executable,
            "python_version": sys.version,
        },
        "toolchain": {
            "gcc": gcc,
            "gxx": gxx,
            "git": git_v,
            "scons": scons_v,
            "libgtest_dev_version": gtest_v,
        },
        "build": {
            "command": build_command,
            "working_directory": "simulator",
            "cxxflags": cxxflags,
            "linkflags": cxxflags,
            "no_storage_argument": 0,
            "no_storage_define_present": False,
            "storage_enabled": True,
            "binary": {
                "path": "simulator/sim",
                "bytes": binary.stat().st_size if binary.exists() else None,
                "sha256": sha256(binary) if binary.exists() else None,
            },
        },
        "original_configs": configs,
        "python_packages": packages,
        "environment_hash": env_hash,
        "environment_hash_inputs": env_core,
    }

    (ENV_DIR / "environment.json").write_text(json.dumps(record, indent=2) + "\n")

    lines = [
        "HBM-PIM Phase 1 - G0 environment",
        "=================================",
        f"generated_utc: {record['generated_utc']}",
        f"project_root: {PROJECT_ROOT}",
        f"environment_hash: {env_hash}",
        "",
        "Upstream source",
        "---------------",
        f"url: {record['upstream']['url']}",
        f"commit: {head}",
        f"tree: {tree}",
        f"commit_date: {commit_date}",
        f"commit_subject: {commit_subject}",
        f"shallow_clone: {shallow}",
        f"worktree_clean: {status == ''}",
        "",
        "Host",
        "----",
        f"os: {record['host']['os_pretty']}",
        f"kernel: {platform.release()}",
        f"arch: {platform.machine()}",
        f"cpu: {cpu_model}",
        f"logical_cpus: {os.cpu_count()}",
        f"memory_bytes: {mem_kb * 1024}",
        f"disk_total_bytes: {disk.total}",
        f"disk_free_bytes: {disk.free}",
        "",
        "Toolchain",
        "---------",
        f"gcc: {gcc}",
        f"g++: {gxx}",
        f"git: {git_v}",
        f"scons: {scons_v}",
        f"libgtest-dev: {gtest_v}",
        f"python: {py_v}",
        f"python_executable: {sys.executable}",
        "",
        "Build",
        "-----",
        f"command: {build_command}",
        f"CXXFLAGS: {cxxflags}",
        "storage: ENABLED (NO_STORAGE=0; -DNO_STORAGE not defined)",
        f"binary_sha256: {record['build']['binary']['sha256']}",
        f"binary_bytes: {record['build']['binary']['bytes']}",
        "",
        "Original config hashes",
        "----------------------",
    ]
    lines += [f"{c['sha256']}  {c['path']}  ({c['bytes']} bytes)" for c in configs]
    lines += ["", "Python packages (pip freeze)", "-----------------------------"] + packages
    lines += ["", "Environment hash inputs", "-----------------------", json.dumps(env_core, indent=2, sort_keys=True)]

    (ENV_DIR / "environment.txt").write_text("\n".join(lines) + "\n")
    print(f"wrote {ENV_DIR / 'environment.txt'}")
    print(f"wrote {ENV_DIR / 'environment.json'}")
    print(f"environment_hash={env_hash}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
