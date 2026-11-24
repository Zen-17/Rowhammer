#!/usr/bin/env python3
"""Run the M0 delivery and stop.  Later milestones are not started."""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(REPO))

from phase3_exp.harness.e1_profile import write_tables
from phase3_exp.harness.interface_manifest import write_manifest
from phase3_exp.harness.paths import CONFIGS, FIGURES, P1, RESULTS, RUNS
from phase3_exp.harness.report import write_report
from phase3_exp.harness.workloads import write_jobs
from phase3_exp.mc.mc_sim import write_smoke
from phase3_exp.plot.pdftext import has_label, pdf_bytes
from phase3_exp.plot.plot_paper import figure_2a


def main() -> int:
    os.chdir(ROOT)
    write_manifest()
    out = RUNS / "M0" / "sim"
    out.mkdir(parents=True, exist_ok=True)
    jobs = out / "jobs.txt"
    write_jobs(jobs)
    build = subprocess.run(["bash", str(ROOT / "scripts/build_phase3.sh")], cwd=ROOT)
    if build.returncode != 0:
        print("PHASE3_BUILD_FAILED", file=sys.stderr)
        return build.returncode
    env = os.environ.copy()
    anaconda_lib = "/opt/data/data/anaconda3/lib"
    env["LD_LIBRARY_PATH"] = anaconda_lib + (
        ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    env.update({
        "PHASE3_DEVICE_INI": str(P1 / "simulator/ini/HBM2_samsung_2M_16B_x64.ini"),
        "PHASE3_SYSTEM_INI": str(CONFIGS / "system_1ch_quiet.ini"),
        "PHASE3_OUT": str(out),
        "PHASE3_JOBS": str(jobs),
    })
    log = out / "gtest.log"
    sim = ROOT / "build/simulator/sim"
    with log.open("w") as handle:
        proc = subprocess.run(
            [str(sim), "--gtest_filter=Phase3M0.*"],
            cwd=out, env=env, stdout=handle, stderr=subprocess.STDOUT,
        )
    try:
        write_tables(out)
    except Exception as exc:
        print(f"E1 assemble failed: {exc}", file=sys.stderr)
    try:
        write_smoke(ROOT / "mc/smoke_test.json")
    except Exception as exc:
        print(f"MC smoke failed: {exc}", file=sys.stderr)
    profile = RESULTS / "E1_benign_profile.csv"
    if profile.exists():
        try:
            figure_2a(profile, FIGURES)
            blob = pdf_bytes(FIGURES / "fig2a_e1_benign.pdf")
            for token in ("W1", "W2", "W4", "GEMV", "DEC", "f_th"):
                if not has_label(blob, token):
                    print(f"fig2(a) text missing {token!r}", file=sys.stderr)
                    proc.returncode = proc.returncode or 1
        except Exception as exc:
            print(f"fig2(a) failed: {exc}", file=sys.stderr)
            proc.returncode = proc.returncode or 1
    # Count gtest results from the log for the report.  A failed workload is
    # still reported; the process stays stopped at G1 either way.
    text = log.read_text(errors="replace")
    passed = "unknown"
    for line in text.splitlines():
        if "PASSED" in line or "FAILED" in line:
            passed = line.strip()
    write_report(passed, str(log.relative_to(ROOT)))
    print(f"DP-M0 written; gtest exit {proc.returncode}; stopped at G1")
    return proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
