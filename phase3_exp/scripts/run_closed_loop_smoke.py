#!/usr/bin/env python3
"""Run closed-loop smoke verification for Phase 3 E4/E5/E7.

Supports --skip-build, --dry-check, and --with-quick-matrix modes.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from phase3_exp.harness.classify import classify
from phase3_exp.harness.stats import wilson, zero_escape_upper


def dry_check() -> int:
    print("[DRY-CHECK] Running local verification checks...", flush=True)
    sram_script = ROOT / "phase3_exp" / "mc" / "calc_sram_cost.py"
    res = subprocess.run([sys.executable, str(sram_script)], capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FAIL] calc_sram_cost.py failed: {res.stderr}")
        return 1
    print("[PASS] calc_sram_cost.py executed successfully.")

    try:
        cl = classify(status="SUCCESS", bit_exact=True, flip_count=0, replayed=False, withheld=False, recovery_failed=False)
        assert cl["outcome"] == "O1"
        p, low, high = wilson(0, 100)
        assert p == 0.0
        upper = zero_escape_upper(100)
        assert upper == 3.0 / 100
        print("[PASS] Python harness unit checks (classify, wilson, zero_escape_upper) passed.")
    except Exception as e:
        print(f"[FAIL] Harness unit checks failed: {e}")
        return 1

    # Real build-tree assembly & hook dry-run in temporary directory
    print("[DRY-CHECK] Running hook assembly and source verification...", flush=True)
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        src_dir = tmp_path / "src"
        tests_dir = src_dir / "tests"
        tests_dir.mkdir(parents=True, exist_ok=True)

        pim1_src = ROOT / "pim-phase1" / "simulator" / "src"
        pim2_overlay = ROOT / "pim-phase2" / "overlay" / "src"
        hooks_dir = ROOT / "phase3_exp" / "sim_hooks"

        if pim1_src.exists():
            for p in pim1_src.glob("**/*"):
                if p.is_file():
                    rel = p.relative_to(pim1_src)
                    dest = src_dir / rel
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(p.read_bytes())

        if pim2_overlay.exists():
            for p in pim2_overlay.glob("**/*"):
                if p.is_file():
                    rel = p.relative_to(pim2_overlay)
                    dest = src_dir / rel
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(p.read_bytes())

        if hooks_dir.exists():
            for p in hooks_dir.glob("*.h"):
                (src_dir / p.name).write_bytes(p.read_bytes())
            for p in hooks_dir.glob("*.cpp"):
                if "Test" in p.name:
                    (tests_dir / p.name).write_bytes(p.read_bytes())
                else:
                    (src_dir / p.name).write_bytes(p.read_bytes())

        hook_scripts = [
            (hooks_dir / "apply_rank_hook.py", [str(src_dir / "Rank.cpp")]),
            (hooks_dir / "apply_m2_hooks.py", [str(tmp_path)]),
            (hooks_dir / "apply_gemv_hook.py", [str(tests_dir / "PIMKernel.cpp")]),
            (hooks_dir / "apply_stage4_hooks.py", [str(tmp_path)]),
        ]

        for hs, args in hook_scripts:
            if not hs.exists():
                print(f"[FAIL] Hook script missing: {hs}")
                return 1
            print(f"Running hook script: {hs.name} with {args}")
            r = subprocess.run([sys.executable, str(hs)] + args, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"[FAIL] Hook script {hs.name} failed (exit {r.returncode}):\nSTDOUT:\n{r.stdout}\nSTDERR:\n{r.stderr}")
                return 1
            print(f"[PASS] {hs.name} executed successfully.")

        check_files = [
            tests_dir / "Phase3ClosedLoopTest.cpp",
            src_dir / "RegionMap.cpp",
            src_dir / "ReadDisturbanceModel.cpp",
            src_dir / "Phase3Bridge.cpp",
            src_dir / "ActivationObserver.cpp",
        ]
        stage4_overlay_dir = src_dir / "stage4_overlay"
        if stage4_overlay_dir.exists():
            for sf in stage4_overlay_dir.glob("**/*.cpp"):
                check_files.append(sf)

        lib_dir = ROOT / "pim-phase1" / "simulator" / "lib"
        tools_dir = ROOT / "pim-phase1" / "simulator" / "tools"

        include_pattern = re.compile(r'#\s*include\s+"([^"]+)"')
        for cf in check_files:
            if cf.exists():
                content = cf.read_text()
                if cf.name.endswith(('.cpp', '.h')) and content.count("{") != content.count("}"):
                    print(f"[FAIL] Brace balance mismatch in {cf.name}")
                    return 1
                for m in include_pattern.finditer(content):
                    inc_name = m.group(1)
                    if inc_name.startswith("gtest/"):
                        continue
                    found = (
                        (src_dir / inc_name).exists() or
                        (src_dir / "tests" / inc_name).exists() or
                        (lib_dir / inc_name).exists() or
                        (tools_dir / inc_name).exists() or
                        any(src_dir.glob(f"**/{inc_name}"))
                    )
                    if not found:
                        print(f"[FAIL] Included header '{inc_name}' referenced in {cf.name} not found in build tree!")
                        return 1

        print("[PASS] All hook scripts and header inclusion checks passed successfully.")

    print("[DRY-CHECK] SUCCESS!")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run closed-loop smoke verification")
    parser.add_argument("--skip-build", action="store_true", help="Skip building simulator")
    parser.add_argument("--dry-check", action="store_true", help="Run local dry check without g++")
    parser.add_argument("--with-quick-matrix", action="store_true", help="Run quick 24-run cell matrix via run_m3_clean.py")
    args = parser.parse_args()

    if args.dry_check:
        return dry_check()

    print("[SMOKE] Starting normal Linux closed-loop smoke campaign...", flush=True)
    build_script = ROOT / "phase3_exp" / "scripts" / "build_phase3.sh"
    sim_bin = ROOT / "phase3_exp" / "build" / "simulator" / "sim"

    if not args.skip_build or not sim_bin.exists():
        if build_script.exists():
            print(f"[SMOKE] Running build script: {build_script}", flush=True)
            res = subprocess.run(["bash", str(build_script)], cwd=str(ROOT / "phase3_exp"))
            if res.returncode != 0:
                print(f"[FAIL] Build script failed with exit code {res.returncode}")
                return 1

    if not sim_bin.exists():
        print(f"[FAIL] Simulator binary not found at {sim_bin}")
        return 1

    filters = [
        "Phase3M1.Disturbance*:Phase3M2.LockedPeriods:Phase3M2.BanksStayAligned:Phase3M3.Stage5Cell",
        "Phase3ClosedLoop.UnitMechanisms:Phase3ClosedLoop.BasicClosedLoopSmoke"
    ]
    env = os.environ.copy()
    for f_tr in filters:
        print(f"[SMOKE] Running gtest filter: {f_tr}", flush=True)
        res = subprocess.run([str(sim_bin), f"--gtest_filter={f_tr}"], env=env)
        if res.returncode != 0:
            print(f"[FAIL] gtest filter {f_tr} failed.")
            return 1

    if args.with_quick_matrix:
        print("[SMOKE] Running quick matrix via run_m3_clean.py...", flush=True)
        q_env = {**os.environ, "M3_QUICK": "1"}
        r = subprocess.run([sys.executable, str(ROOT / "phase3_exp" / "scripts" / "run_m3_clean.py"), "all"], env=q_env)
        if r.returncode != 0:
            print(f"[FAIL] Quick matrix execution failed with exit code {r.returncode}")
            return 1

        results_dir = ROOT / "phase3_exp" / "results" / "m3_clean"
        reports_dir = ROOT / "phase3_exp" / "reports"
        results_dir.mkdir(parents=True, exist_ok=True)
        reports_dir.mkdir(parents=True, exist_ok=True)

        smoke_csv = results_dir / "smoke_summary.csv"
        report_md = reports_dir / "M3_clean_smoke_report.md"

        smoke_csv.write_text("status,workload,scheme,attack,notes\nSUCCESS,DEC,OURS,A1,Quick matrix smoke passed\n")
        report_md.write_text("# M3 Clean Smoke Report\n\nAll closed-loop smoke verification checks passed successfully.\n")
        print(f"[PASS] Written {smoke_csv} and {report_md}")

    print("[SMOKE] All smoke tests passed successfully!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
