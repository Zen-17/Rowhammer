#!/usr/bin/env python3
"""Group-level E2E-0..E2E-7 assertions and the differential gate."""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.phase2.check_differential import check_run  # noqa: E402

RUNS = ROOT / "runs/phase2/l3"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(run_id: str) -> tuple[dict, dict, Path]:
    run_dir = RUNS / run_id
    metrics = json.loads((run_dir / "metrics.json").read_text())
    decision = (json.loads((run_dir / "decision.json").read_text())
                if (run_dir / "decision.json").exists() else {})
    return metrics, decision, run_dir


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(ROOT / "results/phase2/l3/e2e_validation.json"))
    args = parser.parse_args()
    assertions: list[dict] = []

    def check(name: str, group: str, passed: bool, expected, actual) -> None:
        assertions.append({"name": name, "group": group, "pass": bool(passed),
                           "expected": expected, "actual": actual})

    try:
        ref_w1, _dec, ref_w1_dir = load("e2e0_w1_n8_tiles_accon_obson")
        off_w1, _dec2, off_w1_dir = load("e2e0_w1_n8_tiles_accoff_obson")
        obs_w1, _dec3, obs_w1_dir = load("e2e0_w1_n8_tiles_accon_obsoff")
        full_w1, _dec4, full_w1_dir = load("e2e0_w1_n8_full_accon_obson")
        seg_w1, _dec5, seg_w1_dir = load("e2e0_w1_n8_segments_accon_obson")
        check("E2E-0 W1 output exact", "E2E-0", ref_w1.get("output_bit_exact") is True,
              True, ref_w1.get("output_bit_exact"))
        check("E2E-0 W1 ledger entries", "E2E-0", ref_w1.get("ledger_entries") == 8,
              8, ref_w1.get("ledger_entries"))
        check("E2E-0 accounting on/off same cycles", "E2E-0",
              ref_w1.get("producer_cycles") == off_w1.get("producer_cycles"),
              ref_w1.get("producer_cycles"), off_w1.get("producer_cycles"))
        check("E2E-0 accounting on/off same output", "E2E-0",
              sha256(ref_w1_dir / "output.bin") == sha256(off_w1_dir / "output.bin"),
              "same sha256", "compared")
        observer_keys = ("producer_cycles", "end_to_end_cycles", "output_bit_exact",
                         "ledger_entries", "ledger_bytes_est", "published")
        check("E2E-0 observer on/off same metrics", "E2E-0",
              all(ref_w1.get(key) == obs_w1.get(key) for key in observer_keys),
              {key: ref_w1.get(key) for key in observer_keys},
              {key: obs_w1.get(key) for key in observer_keys})
        check("E2E-0 observer on/off same output", "E2E-0",
              sha256(ref_w1_dir / "output.bin") == sha256(obs_w1_dir / "output.bin"),
              "same sha256", "compared")
        check("E2E-0 full vs tiles same cycles", "E2E-0",
              ref_w1.get("producer_cycles") == full_w1.get("producer_cycles"),
              ref_w1.get("producer_cycles"), full_w1.get("producer_cycles"))
        check("E2E-0 full vs tiles same output", "E2E-0",
              sha256(ref_w1_dir / "output.bin") == sha256(full_w1_dir / "output.bin"),
              "same sha256", "compared")
        check("E2E-0 3-segment output exact", "E2E-0",
              seg_w1.get("output_bit_exact") is True
              and sha256(ref_w1_dir / "output.bin") == sha256(seg_w1_dir / "output.bin"),
              True, seg_w1.get("output_bit_exact"))

        for workload, run_id, entries in (
            ("W1", "e2e0_w1_n32_tiles_accon_obson", 32),
            ("W2", "e2e0_w2_n8_tiles_accon_obson", 24),
            ("W2", "e2e0_w2_n32_tiles_accon_obson", 96),
        ):
            metrics, _dec, run_dir = load(run_id)
            check(f"E2E-0 {workload} n={entries} output exact", "E2E-0",
                  metrics.get("output_bit_exact") is True, True,
                  metrics.get("output_bit_exact"))
            check(f"E2E-0 {workload} n={entries} ledger entries", "E2E-0",
                  metrics.get("ledger_entries") == entries, entries,
                  metrics.get("ledger_entries"))
        full_w2, _dec6, full_w2_dir = load("e2e0_w2_n8_full_accon_obson")
        ref_w2, _dec7, ref_w2_dir = load("e2e0_w2_n8_tiles_accon_obson")
        check("E2E-0 W2 full vs tiles same cycles", "E2E-0",
              ref_w2.get("producer_cycles") == full_w2.get("producer_cycles"),
              ref_w2.get("producer_cycles"), full_w2.get("producer_cycles"))
        check("E2E-0 W2 full vs tiles same output", "E2E-0",
              sha256(ref_w2_dir / "output.bin") == sha256(full_w2_dir / "output.bin"),
              "same sha256", "compared")

        for tile in (0, 3, 7):
            metrics, decision, _dir = load(f"e2e1_page_ue_k{tile}")
            check(f"E2E-1 k={tile} status", "E2E-1",
                  metrics.get("observed_status") == "SELECTIVE_RECOVERED",
                  "SELECTIVE_RECOVERED", metrics.get("observed_status"))
            check(f"E2E-1 k={tile} affected", "E2E-1",
                  decision.get("affected") == [tile + 1],
                  [tile + 1], decision.get("affected"))
            check(f"E2E-1 k={tile} restore bytes", "E2E-1",
                  metrics.get("restore_bytes") == 4096, 4096,
                  metrics.get("restore_bytes"))
            check(f"E2E-1 k={tile} output exact", "E2E-1",
                  metrics.get("output_bit_exact") is True, True,
                  metrics.get("output_bit_exact"))
            neg, _decl, _negdir = load(f"e2e1_no_replay_k{tile}")
            check(f"E2E-1 k={tile} NO_REPLAY tile mismatch", "E2E-1",
                  neg.get("output_bit_exact") is False
                  and neg.get("mismatch_tile_ids") == str(tile)
                  and neg.get("mismatch_tiles") == 1,
                  f"tile {tile}", neg.get("mismatch_tile_ids"))

        for tile in (0, 3, 7):
            metrics, decision, _dir = load(f"e2e2_b0_k{tile}")
            check(f"E2E-2 k={tile} status", "E2E-2",
                  metrics.get("observed_status") == "WHOLE_JOB_FALLBACK",
                  "WHOLE_JOB_FALLBACK", metrics.get("observed_status"))
            check(f"E2E-2 k={tile} output exact", "E2E-2",
                  metrics.get("output_bit_exact") is True, True,
                  metrics.get("output_bit_exact"))
            check(f"E2E-2 k={tile} replay tiles", "E2E-2",
                  metrics.get("replay_tiles") == 8, 8, metrics.get("replay_tiles"))

        metrics, decision, _dir = load("e2e3_bank_ue_k3")
        check("E2E-3 affected contains ADD_3", "E2E-3",
              4 in decision.get("affected", []), "4 in affected",
              decision.get("affected"))
        check("E2E-3 output exact", "E2E-3",
              metrics.get("output_bit_exact") is True, True,
              metrics.get("output_bit_exact"))

        metrics, decision, _dir = load("e2e4_ce_b2_k3")
        check("E2E-4 status", "E2E-4",
              metrics.get("observed_status") == "SOURCE_REPAIRED_ONLY",
              "SOURCE_REPAIRED_ONLY", metrics.get("observed_status"))
        check("E2E-4 no replay", "E2E-4", metrics.get("replay_tiles") == 0, 0,
              metrics.get("replay_tiles"))
        check("E2E-4 restore bytes", "E2E-4",
              metrics.get("restore_bytes") == 4096, 4096,
              metrics.get("restore_bytes"))
        check("E2E-4 output exact", "E2E-4",
              metrics.get("output_bit_exact") is True, True,
              metrics.get("output_bit_exact"))
        check("E2E-4 post-recovery read clean", "E2E-4",
              metrics.get("post_recovery_ce_count") == 0, 0,
              metrics.get("post_recovery_ce_count"))

        metrics, decision, _dir = load("e2e5_w2_t_ue_k2")
        affected = decision.get("affected", [])
        check("E2E-5 ADD_2 affected", "E2E-5", 3 in affected, "3 in affected", affected)
        check("E2E-5 MUL_2 affected", "E2E-5", 11 in affected, "11 in affected", affected)
        check("E2E-5 status not untrusted", "E2E-5",
              metrics.get("observed_status") != "OUTSIDE_WINDOW_NO_TRUSTED_INPUT",
              "not untrusted", metrics.get("observed_status"))
        check("E2E-5 output exact", "E2E-5",
              metrics.get("output_bit_exact") is True, True,
              metrics.get("output_bit_exact"))
        neg, _dec, _negdir = load("e2e5_w2_t_no_replay_k2")
        check("E2E-5 NO_REPLAY output wrong", "E2E-5",
              neg.get("output_bit_exact") is False, False,
              neg.get("output_bit_exact"))

        metrics, _dec, _dir = load("e2e6_no_restore_k3")
        check("E2E-6 report during recovery", "E2E-6",
              metrics.get("reports_during_recovery", 0) >= 1, ">=1",
              metrics.get("reports_during_recovery"))
        check("E2E-6 not published", "E2E-6",
              metrics.get("published") is False, False, metrics.get("published"))
        check("E2E-6 recovery failed", "E2E-6",
              metrics.get("recovery_failed") is True, True,
              metrics.get("recovery_failed"))

        metrics, _dec, _dir = load("e2e7_checkpoint_missing_k3")
        check("E2E-7 status", "E2E-7",
              metrics.get("observed_status") == "OUTSIDE_WINDOW_NO_TRUSTED_INPUT",
              "OUTSIDE_WINDOW_NO_TRUSTED_INPUT", metrics.get("observed_status"))
        check("E2E-7 no replay", "E2E-7", metrics.get("replay_tiles") == 0, 0,
              metrics.get("replay_tiles"))
        check("E2E-7 poisoned tile", "E2E-7",
              str(3) in str(metrics.get("poisoned_tiles", "")).split(";"), "3",
              metrics.get("poisoned_tiles"))
        check("E2E-7 host read denied", "E2E-7",
              metrics.get("host_read_denied") is True, True,
              metrics.get("host_read_denied"))

        run_dirs = sorted(path.parent for path in RUNS.glob("*/decision.json"))
        diff_passed = 0
        diff_failures = []
        for run_dir in run_dirs:
            try:
                ok, detail = check_run(run_dir)
            except Exception as error:  # noqa: BLE001
                ok, detail = False, str(error)
            if ok:
                diff_passed += 1
            else:
                diff_failures.append({"run": run_dir.name, "detail": detail})
        check("DIFF all decisions reproduced", "DIFF",
              diff_passed == len(run_dirs) and len(run_dirs) > 0,
              f"{len(run_dirs)}/{len(run_dirs)}", f"{diff_passed}/{len(run_dirs)}")
        (ROOT / "results/phase2/l3/differential.json").write_text(
            json.dumps({"runs": len(run_dirs), "passed": diff_passed,
                        "failures": diff_failures,
                        "pass": diff_passed == len(run_dirs) and len(run_dirs) > 0},
                       indent=2) + "\n")
    except Exception as error:  # noqa: BLE001
        check("E2E suite readable", "E2E", False, "all runs present", str(error))

    all_pass = all(assertion["pass"] for assertion in assertions) and bool(assertions)
    payload = {"pass": all_pass, "assertions": assertions}
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2) + "\n")
    for assertion in assertions:
        if not assertion["pass"]:
            print(f"FAIL {assertion['group']} {assertion['name']}: "
                  f"expected={assertion['expected']} actual={assertion['actual']}")
    print(f"E2E_ASSERTIONS={sum(1 for a in assertions if a['pass'])}/{len(assertions)}")
    print(f"E2E_VALIDATION={'PASS' if all_pass else 'FAIL'}")
    return 0 if all_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())
