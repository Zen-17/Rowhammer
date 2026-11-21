#!/usr/bin/env python3
"""Build the Phase 2 v2.1 validation ledger."""
from __future__ import annotations

import csv
import json
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PY = ROOT / ".venv/bin/python"
SIM = ROOT / "simulator/sim"
OUT = ROOT / "results/phase2/v21/validation.json"
WHITELIST = {"Phase1Feeder.cpp", "Phase1Feeder.h", "tests/PIMKernel.cpp", "tests/PIMKernel.h"}
FIGURES = ["p2f1_selectivity", "p2f2_breakdown", "p2f3_release", "p2f4_radius",
           "p2f5_status", "p2f6_validation", "p2f7_late"]


def run(command: list[str], cwd: Path = ROOT, env: dict | None = None):
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True, env=env)


def main() -> int:
    assertions: list[dict] = []

    def check(name: str, group: str, passed: bool, expected, actual) -> None:
        assertions.append({"name": name, "group": group, "pass": bool(passed),
                           "expected": expected, "actual": actual})

    calibration = json.loads((ROOT / "configs/phase2/v21/calibration.json").read_text())
    for key, item in calibration["reference_check"].items():
        check(f"calibration.{key}", "F0", item["pass"],
              f"{item['expected']} +/- 1", item["observed"])
    for name, item in calibration["kernel_fits"].items():
        check(f"calibration.kernel.{name}.linear", "F6", not item["nonlinear"],
              "max relative residual <= 5%",
              f"{item['max_relative_residual_percent']:.2f}%")

    before_rows = max(0, len((ROOT / "results/phase2/regress_before/run_metrics.csv")
                             .read_text().strip().splitlines()) - 1)
    check("regression.before.240", "F2", before_rows == 240, 240, before_rows)
    after_rows = max(0, len((ROOT / "results/phase2/v21/regress_after/run_metrics.csv")
                            .read_text().strip().splitlines()) - 1)
    check("regression.after.240", "F2", after_rows == 240, 240, after_rows)
    compare = json.loads((ROOT / "results/phase2/v21/regress_after/compare.json").read_text())
    check("regression.after.zero_drift", "F2", compare["pass"], "REGRESSION=PASS",
          f"checked={compare['cases_checked']} failures={len(compare['failures'])}")

    isolation = run(["bash", str(ROOT / "scripts/phase2/check_isolation.sh"), "--require-all"])
    check("isolation.grep", "F1", isolation.returncode == 0, "ISOLATION=PASS",
          isolation.stdout.strip().splitlines()[-1] if isolation.stdout else "")

    pytest_engine = run([str(PY), "-m", "pytest", "tests/phase2/test_engine.py", "-q"])
    check("python.U1_U24", "F1", pytest_engine.returncode == 0, "25 passed",
          pytest_engine.stdout.strip().splitlines()[-1] if pytest_engine.stdout else "")
    pytest_addr = run([str(PY), "-m", "pytest", "tests/phase2/test_address_vectors.py", "-q"])
    check("python.address_vectors_match", "F2", pytest_addr.returncode == 0, "all pass",
          pytest_addr.stdout.strip().splitlines()[-1] if pytest_addr.stdout else "")

    env = os.environ.copy()
    env["PHASE2_ADDRESS_OUT"] = str(ROOT / "results/phase2/v21/l1/address_vectors.csv")
    cpp_tests = run([str(SIM), "--gtest_filter=Phase2Engine.*:Phase2AddressVectors.*"],
                    cwd=ROOT / "simulator", env=env)
    check("cpp.U1_U24_and_address_export", "F2", cpp_tests.returncode == 0,
          "26 tests passed",
          cpp_tests.stdout.strip().splitlines()[-1] if cpp_tests.stdout else "")
    vector_rows = max(0, len((ROOT / "results/phase2/v21/l1/address_vectors.csv")
                             .read_text().strip().splitlines()) - 1)
    check("cpp.address_vectors.count", "F2", vector_rows >= 1000, ">=1000", vector_rows)

    diff = run(["diff", "-rq", str(ROOT / "runs/phase2/stage0/src_snapshot"),
                str(ROOT / "simulator/src")])
    modified = set()
    for line in diff.stdout.splitlines():
        if line.startswith("Files "):
            target = line.split(" and ", 1)[1].split(" differ", 1)[0]
            modified.add(str(Path(target).relative_to(ROOT / "simulator/src")))
    check("source.patch.whitelist", "F2", modified == WHITELIST,
          sorted(WHITELIST), sorted(modified))
    check("source.patch.exists", "F2",
          (ROOT / "environment/phase2_source.patch").stat().st_size > 0,
          ">0 bytes", (ROOT / "environment/phase2_source.patch").stat().st_size)

    l2 = list(csv.DictReader((ROOT / "results/phase2/v21/l2/cases.csv").open(newline="")))
    counts: dict[str, int] = {}
    for row in l2:
        counts[row["kind"]] = counts.get(row["kind"], 0) + 1
    expected_counts = {"main": 45360, "window": 3780, "late": 7200,
                       "radius": 900, "release": 2160, "status": 270, "ledger": 36}
    check("l2.matrix_counts", "F3", counts == expected_counts,
          json.dumps(expected_counts, sort_keys=True), json.dumps(counts, sort_keys=True))
    main_rows = [row for row in l2 if row["kind"] == "main"]
    main_wrong = [row for row in main_rows if row["correct"] not in ("1", "True", "true")]
    check("l2.main_correct", "F3", len(main_rows) == 45360 and not main_wrong,
          "45360/45360", f"{len(main_rows) - len(main_wrong)}/{len(main_rows)}")
    release = [row for row in l2 if row["kind"] == "release"]
    negative = [row for row in release if float(row["overhead_fraction"]) < 0]
    check("l2.release_no_negative", "F3", not negative, "all >= 0",
          f"min={min(float(row['overhead_fraction']) for row in release):.6f}")

    status = [row for row in l2 if row["kind"] == "status"]
    def status_count(scenario: str, state: str) -> int:
        return sum(1 for row in status if row["status_scenario"] == scenario
                   and row["status"] == state)
    check("l2.delta_full_selective", "F3",
          status_count("DELTA_FULL_1.0", "SELECTIVE_RECOVERED") == 30,
          "30/30", status_count("DELTA_FULL_1.0", "SELECTIVE_RECOVERED"))
    check("l2.delta_short_page_selective", "F3",
          status_count("DELTA_SHORT_PAGE_0.5", "SELECTIVE_RECOVERED") == 30,
          "30/30", status_count("DELTA_SHORT_PAGE_0.5", "SELECTIVE_RECOVERED"))
    page_short = [row for row in status
                  if row["status_scenario"] == "DELTA_SHORT_PAGE_0.5"]
    check("l2.delta_short_page_correct", "F3",
          all(row["correct"] in ("1", "True", "true") for row in page_short),
          "30/30", sum(1 for row in page_short if row["correct"] in ("1", "True", "true")))
    check("l2.delta_short_bank_actual", "F3",
          status_count("DELTA_SHORT_BANK_0.5", "SELECTIVE_RECOVERED") == 21
          and status_count("DELTA_SHORT_BANK_0.5", "WHOLE_JOB_FALLBACK") == 9,
          "21 selective / 9 fallback",
          f"{status_count('DELTA_SHORT_BANK_0.5', 'SELECTIVE_RECOVERED')} / "
          f"{status_count('DELTA_SHORT_BANK_0.5', 'WHOLE_JOB_FALLBACK')}")
    check("l2.same_cycle_selective", "F3",
          status_count("SAME_CYCLE_RELEASE", "SELECTIVE_RECOVERED") == 30,
          "30/30", status_count("SAME_CYCLE_RELEASE", "SELECTIVE_RECOVERED"))

    radius = [row for row in l2 if row["kind"] == "radius"]
    def radius_cover(distance: str, policy: str) -> float:
        subset = [row for row in radius
                  if row["distance_class"] == distance and row["policy"] == policy]
        return (sum(1 for row in subset if row["silent_repaired"] in ("1", "True", "true"))
                / len(subset)) if subset else 0.0
    check("l2.radius_pareto", "F3",
          radius_cover("same_row", "B2R-0") == 1.0
          and radius_cover("plus1", "B2R-0") == 0.0
          and radius_cover("plus1", "B2R-1") == 1.0
          and radius_cover("far", "B2R-2") == 0.0
          and radius_cover("far", "B0") == 1.0,
          "B2R-k covers k rows; far only B0",
          {f"{d}/{p}": radius_cover(d, p) for d, p in
           [("same_row", "B2R-0"), ("plus1", "B2R-0"), ("plus1", "B2R-1"),
            ("far", "B2R-2"), ("far", "B0")]})

    stats_rows = max(0, len((ROOT / "results/phase2/v21/statistics.csv")
                             .read_text().strip().splitlines()) - 1)
    check("l2.statistics_nonempty", "F3", stats_rows > 0, ">0 rows", stats_rows)

    l3_run = json.loads((ROOT / "results/phase2/v21/l3/run_validation.json").read_text())
    check("l3.run_all_cases", "F4", l3_run["pass"], "1120/1120 reconciled",
          json.dumps(l3_run["counts"], sort_keys=True))
    e2e = json.loads((ROOT / "results/phase2/v21/l3/e2e_validation.json").read_text())
    for item in e2e["assertions"]:
        check(item["name"], item["group"], item["pass"], item["expected"], item["actual"])
    differential = json.loads((ROOT / "results/phase2/v21/l3/differential.json").read_text())
    check("l3.differential", "F4", differential["pass"],
          f"{differential['runs']}/{differential['runs']}",
          f"{differential['passed']}/{differential['runs']}")

    counter = json.loads((ROOT / "results/phase2/v21/l3/f2_counterfactual_summary.json")
                         .read_text())
    check("l3.f2_counterfactual", "F5", counter["rows"] == 360
          and counter["counts"].get("FAIL", 0) == 0,
          "360 rows, 0 FAIL", json.dumps(counter["counts"], sort_keys=True))
    check("l3.f2_masked_measured", "F5", counter["counts"].get("MASKED", 0) == 160,
          "160 MASKED", counter["counts"].get("MASKED", 0))

    p2f6 = json.loads((ROOT / "results/phase2/v21/l3/p2f6_summary.json").read_text())
    check("f6.validation_mape_reported", "F6",
          len(p2f6.get("by_workload_metric", {})) == 4, "4 workload/metric cells",
          json.dumps(p2f6.get("by_workload_metric", {}), sort_keys=True))

    for name in FIGURES:
        for suffix in ("pdf", "svg", "png"):
            path = ROOT / f"figures/phase2/v21/{name}.{suffix}"
            check(f"figure.{name}.{suffix}", "F7", path.exists(), "exists", path.exists())
    check("figures.captions", "F7",
          (ROOT / "figures/phase2/v21/captions.md").exists(), "exists", True)
    check("figures.manifest", "F7",
          (ROOT / "figures/phase2/v21/plot_manifest.json").exists(), "exists", True)
    for table in ("p2t1_ledger", "p2t2_comparison"):
        for suffix in ("md", "tex"):
            path = ROOT / f"figures/phase2/v21/{table}.{suffix}"
            check(f"table.{table}.{suffix}", "F7", path.exists(), "exists", path.exists())
    check("report.phase2_results_v21", "F7",
          (ROOT / "report/phase2_results_v21.md").exists(), "exists", True)
    check("reproduce.script.v21", "F7",
          (ROOT / "scripts/phase2/reproduce_phase2.sh").exists(), "exists", True)

    all_pass = all(item["pass"] for item in assertions) and bool(assertions)
    payload = {"schema_version": 2, "assertions": assertions,
               "count": len(assertions),
               "passed": sum(1 for item in assertions if item["pass"]),
               "pass": all_pass}
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
