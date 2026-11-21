#!/usr/bin/env python3
"""Build the final Phase 2 validation ledger (name / pass / expected / actual)."""
from __future__ import annotations

import json
import os
import csv
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PY = ROOT / ".venv/bin/python"
SIM = ROOT / "simulator/sim"
OUT = ROOT / "results/phase2/validation.json"
WHITELIST = {"Phase1Feeder.cpp", "Phase1Feeder.h", "tests/PIMKernel.cpp", "tests/PIMKernel.h"}
EXPECTED_FIGURES = [
    "p2f1_selectivity", "p2f2_breakdown", "p2f3_release", "p2f4_radius",
    "p2f5_status", "p2f6_validation",
]


def run(command: list[str], cwd: Path = ROOT, env: dict | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True, env=env)


def main() -> int:
    assertions: list[dict] = []

    def check(name: str, group: str, passed: bool, expected, actual) -> None:
        assertions.append({"name": name, "group": group, "pass": bool(passed),
                           "expected": expected, "actual": actual})

    calibration = json.loads((ROOT / "configs/phase2/calibration.json").read_text())
    for key, item in calibration["reference_check"].items():
        check(f"calibration.{key}", "STAGE0", item["pass"],
              f"{item['expected']} +/- 1", item["observed"])

    before = (ROOT / "results/phase2/regress_before/run_metrics.csv").read_text()
    before_rows = max(0, len(before.strip().splitlines()) - 1)
    check("regression.before.240", "STAGE0", before_rows == 240, 240, before_rows)

    isolation = run(["bash", str(ROOT / "scripts/phase2/check_isolation.sh"), "--require-all"])
    check("isolation.grep", "STAGE2", isolation.returncode == 0,
          "ISOLATION=PASS", isolation.stdout.strip().splitlines()[-1] if isolation.stdout else "")

    pytest_engine = run([str(PY), "-m", "pytest", "tests/phase2/test_engine.py", "-q"])
    check("python.U1_U15", "STAGE2", pytest_engine.returncode == 0,
          "all pass", pytest_engine.stdout.strip().splitlines()[-1]
          if pytest_engine.stdout else "")

    aggregate = json.loads((ROOT / "results/phase2/l2/aggregate_validation.json").read_text())
    check("l2.matrix_counts", "STAGE3", aggregate["pass"],
          "45360 main / 900 radius / 2160 release / 210 status / 36 ledger",
          json.dumps(aggregate["counts"], sort_keys=True))
    with (ROOT / "results/phase2/l2/cases.csv").open(newline="") as handle:
        l2_rows = list(csv.DictReader(handle))
    main_rows = [row for row in l2_rows if row["kind"] == "main"]
    main_wrong = [row for row in main_rows if row["correct"] not in ("1", "True", "true")]
    check("l2.main_correct", "STAGE3", len(main_rows) == 45360 and not main_wrong,
          "45360/45360", f"{len(main_rows) - len(main_wrong)}/{len(main_rows)}")
    with (ROOT / "results/phase2/figure_data/p2f5_status.csv").open(newline="") as handle:
        status_rows = list(csv.DictReader(handle))
    status_counts: dict[tuple[str, str], int] = {}
    for row in status_rows:
        status_counts[(row["scenario"], row["status"])] = \
            status_counts.get((row["scenario"], row["status"]), 0) + 1
    expected_states = {
        "NORMAL_UE": "SELECTIVE_RECOVERED",
        "CE": "SOURCE_REPAIRED_ONLY",
        "DELTA_SHORT_0.5": "SOURCE_REPAIRED_ONLY",
        "DELTA_FULL_1.0": "PARTIAL_WITH_ESCAPE",
        "CHECKPOINT_MISSING": "OUTSIDE_WINDOW_NO_TRUSTED_INPUT",
        "LEDGER_TRUNCATED": "WHOLE_JOB_FALLBACK",
        "REPORT_OVERFLOW": "WHOLE_JOB_FALLBACK",
    }
    status_ok = all(status_counts.get((scenario, status), 0) == 30
                    for scenario, status in expected_states.items())
    check("l2.status_distribution", "STAGE3", status_ok,
          "each scenario 30/30 expected status",
          {scenario: {status: status_counts.get((scenario, status), 0)
                      for status in set(expected_states.values())}
           for scenario in expected_states})
    with (ROOT / "results/phase2/figure_data/p2f4_radius.csv").open(newline="") as handle:
        radius_rows = list(csv.DictReader(handle))
    coverage: dict[tuple[str, str], float] = {}
    radius_groups: dict[tuple[str, str], list[int]] = {}
    for row in radius_rows:
        radius_groups.setdefault((row["distance_class"], row["policy"]), []).append(
            int(row["silent_repaired"]))
    for key, values in radius_groups.items():
        coverage[key] = sum(values) / len(values)
    check("l2.radius_pareto", "STAGE3",
          coverage.get(("same_row", "B2R-0")) == 1.0
          and coverage.get(("plus1", "B2R-1")) == 1.0
          and coverage.get(("plus2", "B2R-2")) == 1.0
          and coverage.get(("far", "B2R-2")) == 0.0,
          "B2R-k covers k rows, far uncovered",
          {f"{k[0]}/{k[1]}": v for k, v in coverage.items()
           if k[1] in ("B2R-0", "B2R-1", "B2R-2")})
    with (ROOT / "results/phase2/figure_data/p2f3_release.csv").open(newline="") as handle:
        release_rows = list(csv.DictReader(handle))
    l2_release = [row for row in release_rows if row["source"] == "L2"
                  and row["workload"] == "W1" and row["release"] == "GLOBAL_HOLD"
                  and row["n_tiles"] in ("1", "8")]
    def release_mean(ratio: float) -> float:
        values = [float(row["overhead_fraction"]) for row in l2_release
                  if abs(float(row["delta_over_job"]) - ratio) < 0.01]
        return sum(values) / len(values)
    check("l2.release_calibration_0.25", "STAGE3",
          abs(release_mean(0.25) * 100 - 10.21) <= 5.0,
          "10.21 +/- 5 pp", f"{release_mean(0.25) * 100:.2f}%")
    check("l2.release_calibration_1.0", "STAGE3",
          abs(release_mean(1.0) * 100 - 43.96) <= 5.0,
          "43.96 +/- 5 pp", f"{release_mean(1.0) * 100:.2f}%")
    stats_text = (ROOT / "results/phase2/statistics.csv").read_text()
    check("l2.statistics_nonempty", "STAGE3",
          len(stats_text.strip().splitlines()) > 1, ">0 rows",
          max(0, len(stats_text.strip().splitlines()) - 1))

    env = os.environ.copy()
    env["PHASE2_ADDRESS_OUT"] = str(ROOT / "results/phase2/l1/address_vectors.csv")
    cpp_tests = run([str(SIM), "--gtest_filter=Phase2Engine.*:Phase2AddressVectors.*"],
                    cwd=ROOT / "simulator", env=env)
    check("cpp.U1_U15_and_address_export", "STAGE4", cpp_tests.returncode == 0,
          "PASSED", cpp_tests.stdout.strip().splitlines()[-1] if cpp_tests.stdout else "")
    vector_rows = max(0, len((ROOT / "results/phase2/l1/address_vectors.csv")
                             .read_text().strip().splitlines()) - 1)
    check("cpp.address_vectors.count", "STAGE4", vector_rows >= 1000, ">=1000", vector_rows)
    pytest_addr = run([str(PY), "-m", "pytest", "tests/phase2/test_address_vectors.py", "-q"])
    check("python.address_vectors_match", "STAGE4", pytest_addr.returncode == 0,
          "all pass", pytest_addr.stdout.strip().splitlines()[-1]
          if pytest_addr.stdout else "")

    compare = json.loads((ROOT / "results/phase2/regress_after/compare.json").read_text())
    check("regression.after.zero_drift", "STAGE4", compare["pass"],
          "REGRESSION=PASS", f"checked={compare['cases_checked']} failures={len(compare['failures'])}")

    diff = run(["diff", "-rq", str(ROOT / "runs/phase2/stage0/src_snapshot"),
                str(ROOT / "simulator/src")])
    modified = set()
    for line in diff.stdout.splitlines():
        if line.startswith("Files "):
            target = line.split(" and ", 1)[1].split(" differ", 1)[0]
            modified.add(str(Path(target).relative_to(ROOT / "simulator/src")))
    check("source.patch.whitelist", "STAGE4", modified == WHITELIST,
          sorted(WHITELIST), sorted(modified))
    check("source.patch.exists", "STAGE4",
          (ROOT / "environment/phase2_source.patch").stat().st_size > 0,
          ">0 bytes", (ROOT / "environment/phase2_source.patch").stat().st_size)

    l3_run = json.loads((ROOT / "results/phase2/l3/run_validation.json").read_text())
    check("l3.run_all_cases", "STAGE5", l3_run["pass"], "745/745 reconciled",
          json.dumps(l3_run["counts"], sort_keys=True))
    e2e = json.loads((ROOT / "results/phase2/l3/e2e_validation.json").read_text())
    for item in e2e["assertions"]:
        check(item["name"], item["group"], item["pass"], item["expected"], item["actual"])
    differential = json.loads((ROOT / "results/phase2/l3/differential.json").read_text())
    check("l3.differential", "DIFF", differential["pass"],
          f"{differential['runs']}/{differential['runs']}",
          f"{differential['passed']}/{differential['runs']}")

    p2f6 = json.loads((ROOT / "results/phase2/l3/p2f6_summary.json").read_text())
    check("l3.p2f6.validation_mape_reported", "STAGE6", p2f6["validation_rows"] >= 10,
          ">=10 validation rows",
          f"rows={p2f6['validation_rows']} MAPE={p2f6['validation_mape_percent']:.2f}%")

    for name in EXPECTED_FIGURES:
        for suffix in ("pdf", "svg", "png"):
            path = ROOT / f"figures/phase2/{name}.{suffix}"
            check(f"figure.{name}.{suffix}", "STAGE7", path.exists(), "exists", path.exists())
        gray = ROOT / f"figures/phase2/{name}_gray.png"
        check(f"figure.{name}.gray", "STAGE7", gray.exists(), "exists", gray.exists())
    check("figures.captions", "STAGE7",
          (ROOT / "figures/phase2/captions.md").exists(), "exists", True)
    check("figures.manifest", "STAGE7",
          (ROOT / "figures/phase2/plot_manifest.json").exists(), "exists", True)
    check("report.phase2_results", "STAGE7",
          (ROOT / "report/phase2_results.md").exists(), "exists", True)
    check("reproduce.script", "STAGE7",
          (ROOT / "scripts/phase2/reproduce_phase2.sh").exists(), "exists", True)

    all_pass = all(item["pass"] for item in assertions) and bool(assertions)
    payload = {
        "schema_version": 1,
        "assertions": assertions,
        "count": len(assertions),
        "passed": sum(1 for item in assertions if item["pass"]),
        "pass": all_pass,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(payload, indent=2) + "\n")
    for item in assertions:
        if not item["pass"]:
            print(f"FAIL {item['group']} {item['name']}: expected={item['expected']} "
                  f"actual={item['actual']}")
    print(f"VALIDATION_ASSERTIONS={payload['passed']}/{payload['count']}")
    print(f"VALIDATION={'PASS' if all_pass else 'FAIL'}")
    return 0 if all_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())
