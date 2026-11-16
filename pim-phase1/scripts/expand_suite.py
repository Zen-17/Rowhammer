#!/usr/bin/env python3
"""Expand the declarative paper matrix into a flat, hash-addressed manifest."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import yaml

ROOT = Path(__file__).resolve().parent.parent
SIM_DIR = ROOT / "simulator"
DEVICE_INI = SIM_DIR / "ini/HBM2_samsung_2M_16B_x64.ini"
SYSTEM_1CH = ROOT / "configs/phase1_system_1ch.ini"
SYSTEM_2CH = ROOT / "configs/phase1_system_2ch.ini"
RUN_ROOT = ROOT / "runs/paper"
CONFIG_ROOT = ROOT / "configs/generated/paper"
MANIFEST = ROOT / "configs/generated/paper-manifest.json"
UPSTREAM_COMMIT = "3703d1f19c8f027360cc33a3243eb271e3bb6898"

GROUP_KEYS = {
    "P1A": {"fixture", "profiles", "paths", "faults", "banks", "granularity",
            "transport_delay_cycles", "service_interval_cycles", "target_lane",
            "data_bit_in_lane", "fault_bit2", "watchdog_cycles"},
    "P1B": {"fixture", "profile", "path", "fault", "banks", "granularities",
            "transport_delays", "service_intervals", "target_lane", "data_bit_in_lane",
            "fault_bit2", "watchdog_cycles", "burst_reads", "burst_interval", "burst_delay"},
    "P1C": {"fixture", "profile", "granularity", "fault_kind", "n_tiles", "consumers",
            "seed_start", "seed_stop", "delay_ratios", "hold_ratios", "handler_delay_cycles",
            "service_interval_cycles", "report_queue_capacity", "watchdog_cycles",
            "negative_profiles", "negative_consumers", "negative_holds",
            "negative_seed_start", "negative_seed_stop"},
    "P1D": {"fixture", "synthetic_scenarios", "feeder_limits", "handler_delays",
            "observation_cycles", "watchdog_cycles", "actual_ce", "two_channel"},
    "P1E": {"fixture", "n_tiles", "seed_start", "seed_stop", "trigger", "handler_delay_cycles",
            "watchdog_cycles", "status_cases", "negative_controls", "negative_control_watchdog"},
    "P1F_COST": {"fixture", "n_tiles", "consumers", "seed_start", "seed_stop", "policies",
                 "watchdog_cycles"},
    "P1F_ROBUST": {"fixture", "queue_capacities", "service_intervals", "drop_rates",
                   "repeat_reads", "transport_delay_cycles", "watchdog_cycles"},
}


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_group(name: str, group: dict) -> None:
    if name not in GROUP_KEYS:
        raise ValueError(f"unknown suite group: {name}")
    unknown = set(group) - GROUP_KEYS[name]
    if unknown:
        raise ValueError(f"{name}: unknown keys: {sorted(unknown)}")
    missing = GROUP_KEYS[name] - set(group)
    if missing:
        raise ValueError(f"{name}: missing keys: {sorted(missing)}")
    if not isinstance(group["fixture"], str):
        raise ValueError(f"{name}: fixture must be a string")


def base_params(experiment: str) -> dict:
    return {
        "experiment": experiment,
        "device_ini": str(DEVICE_INI),
        "system_ini": str(SYSTEM_1CH),
    }


def target_burst_for(seed: int, n_tiles: int) -> tuple[int, str]:
    dim = n_tiles * 128
    segment = seed % 3
    low = (dim * segment) // 3
    high = (dim * (segment + 1)) // 3
    rng = np.random.Generator(np.random.PCG64(seed))
    value = int(rng.integers(low, high))
    return value, ["first", "middle", "last"][segment]


def add_case(cases: list[dict], seen: set[str], group_key: str, group_label: str,
             fixture: str, params: dict, case_meta: dict, expected_negative: bool = False,
             expected_status: str | None = None) -> None:
    canonical_params = {k: v for k, v in params.items() if k != "output_dir"}
    canonical = json.dumps({
        "group_key": group_key,
        "fixture": fixture,
        "params": canonical_params,
    }, sort_keys=True, separators=(",", ":"))
    run_id = hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:16]
    if run_id in seen:
        raise ValueError(f"run_id collision: {run_id}")
    seen.add(run_id)
    out = RUN_ROOT / run_id
    params = dict(params)
    params["output_dir"] = str(out)
    meta = dict(case_meta)
    meta["out"] = str(out)
    case = {
        "run_id": run_id,
        "group": group_label,
        "group_key": group_key,
        "fixture": fixture,
        "params": {k: str(v) for k, v in params.items()},
        "case": meta,
        "expected_negative": expected_negative,
    }
    if expected_status is not None:
        case["expected_status"] = expected_status
    cases.append(case)


def expand_p1a(group: dict, cases: list[dict], seen: set[str]) -> None:
    for profile in group["profiles"]:
        for path in group["paths"]:
            for fault in group["faults"]:
                for bank in group["banks"]:
                    params = base_params("G4")
                    params.update({
                        "profile": profile,
                        "path": path,
                        "fault_kind": fault,
                        "target_bank": bank,
                        "target_lane": group["target_lane"],
                        "data_bit_in_lane": group["data_bit_in_lane"],
                        "fault_bit2": group["fault_bit2"],
                        "granularity": group["granularity"],
                        "transport_delay_cycles": group["transport_delay_cycles"],
                        "service_interval_cycles": group["service_interval_cycles"],
                        "report_queue_capacity": 64,
                        "repeat_reads": group.get("burst_reads", 32),
                        "watchdog_cycles": group["watchdog_cycles"],
                    })
                    meta = {
                        "case_id": f"P1A_{profile}_{path}_{fault}_bank{bank}",
                        "profile": profile, "path": path, "fault": fault, "bank": bank,
                        "granularity": group["granularity"],
                        "delay": group["transport_delay_cycles"],
                        "interval": group["service_interval_cycles"],
                    }
                    add_case(cases, seen, "P1A", "P1-A", group["fixture"], params, meta)


def expand_p1b(group: dict, cases: list[dict], seen: set[str]) -> None:
    for bank in group["banks"]:
        for granularity in group["granularities"]:
            for delay in group["transport_delays"]:
                for interval in group["service_intervals"]:
                    params = base_params("G4")
                    params.update({
                        "profile": group["profile"],
                        "path": group["path"],
                        "fault_kind": group["fault"],
                        "target_bank": bank,
                        "target_lane": group["target_lane"],
                        "data_bit_in_lane": group["data_bit_in_lane"],
                        "fault_bit2": group["fault_bit2"],
                        "granularity": granularity,
                        "transport_delay_cycles": delay,
                        "service_interval_cycles": interval,
                        "report_queue_capacity": 64,
                        "repeat_reads": group["burst_reads"],
                        "watchdog_cycles": group["watchdog_cycles"],
                    })
                    meta = {
                        "case_id": f"P1B_bank{bank}_{granularity}_d{delay}_i{interval}",
                        "profile": group["profile"], "path": group["path"],
                        "fault": group["fault"], "bank": bank,
                        "granularity": granularity, "delay": delay, "interval": interval,
                    }
                    add_case(cases, seen, "P1B", "P1-B", group["fixture"], params, meta)
    for bank in group["banks"]:
        params = base_params("G4")
        params.update({
            "profile": group["profile"],
            "path": "HREAD_BURST",
            "fault_kind": group["fault"],
            "target_bank": bank,
            "target_lane": group["target_lane"],
            "data_bit_in_lane": group["data_bit_in_lane"],
            "fault_bit2": group["fault_bit2"],
            "granularity": "bank",
            "transport_delay_cycles": group["burst_delay"],
            "service_interval_cycles": group["burst_interval"],
            "report_queue_capacity": 64,
            "repeat_reads": group["burst_reads"],
            "watchdog_cycles": group["watchdog_cycles"],
        })
        meta = {
            "case_id": f"P1B_BURST_bank{bank}_bank_d{group['burst_delay']}"
                       f"_i{group['burst_interval']}",
            "profile": group["profile"], "path": "HREAD_BURST", "fault": group["fault"],
            "bank": bank, "granularity": "bank", "delay": group["burst_delay"],
            "interval": group["burst_interval"], "repeat_reads": group["burst_reads"],
        }
        add_case(cases, seen, "P1B", "P1-B", group["fixture"], params, meta)


def expand_p1c(group: dict, calibration: dict, cases: list[dict], seen: set[str]) -> None:
    t_ref = calibration["p1c_t_ref"]
    for n_tiles in group["n_tiles"]:
        tref = int(t_ref[str(n_tiles)])
        for consumer in group["consumers"]:
            for seed in range(group["seed_start"], group["seed_stop"] + 1):
                target_burst, segment = target_burst_for(seed, n_tiles)
                for delay_ratio in group["delay_ratios"]:
                    for hold_ratio in group["hold_ratios"]:
                        delay = 0 if delay_ratio == 0 else math.ceil(delay_ratio * tref)
                        hold = 0 if hold_ratio == 0 else math.ceil(hold_ratio * tref)
                        params = base_params("P1C")
                        params.update({
                            "profile": group["profile"],
                            "granularity": group["granularity"],
                            "fault_kind": group["fault_kind"],
                            "n_tiles": n_tiles,
                            "seed": seed,
                            "consumer": consumer,
                            "target_burst": target_burst,
                            "transport_delay_cycles": delay,
                            "hold_cycles": hold,
                            "t_ref_cycles": tref,
                            "handler_delay_cycles": group["handler_delay_cycles"],
                            "service_interval_cycles": group["service_interval_cycles"],
                            "report_queue_capacity": group["report_queue_capacity"],
                            "watchdog_cycles": group["watchdog_cycles"],
                        })
                        meta = {
                            "case_id": f"P1C_nt{n_tiles}_{consumer.lower()}_s{seed}"
                                       f"_d{delay_ratio}_h{hold_ratio}",
                            "n_tiles": n_tiles, "seed": seed, "consumer": consumer,
                            "target_burst": target_burst, "target_segment": segment,
                            "delay_ratio": delay_ratio, "hold_ratio": hold_ratio,
                            "transport_delay_cycles": delay, "hold_cycles": hold,
                            "t_ref_cycles": tref,
                        }
                        add_case(cases, seen, "P1C", "P1-C", group["fixture"], params, meta)
    for profile in group["negative_profiles"]:
        for consumer in group["negative_consumers"]:
            tref = int(t_ref["1"])
            for hold_ratio in group["negative_holds"]:
                hold = 0 if hold_ratio == 0 else math.ceil(hold_ratio * tref)
                for seed in range(group["negative_seed_start"],
                                  group["negative_seed_stop"] + 1):
                    target_burst, segment = target_burst_for(seed, 1)
                    params = base_params("P1C")
                    params.update({
                        "profile": profile,
                        "granularity": group["granularity"],
                        "fault_kind": group["fault_kind"],
                        "n_tiles": 1,
                        "seed": seed,
                        "consumer": consumer,
                        "target_burst": target_burst,
                        "transport_delay_cycles": 0,
                        "hold_cycles": hold,
                        "t_ref_cycles": tref,
                        "handler_delay_cycles": group["handler_delay_cycles"],
                        "service_interval_cycles": group["service_interval_cycles"],
                        "report_queue_capacity": group["report_queue_capacity"],
                        "watchdog_cycles": group["watchdog_cycles"],
                    })
                    meta = {
                        "case_id": f"P1C_CTRL_{profile}_{consumer.lower()}_s{seed}"
                                   f"_h{hold_ratio}",
                        "n_tiles": 1, "seed": seed, "consumer": consumer,
                        "profile": profile, "target_burst": target_burst,
                        "target_segment": segment, "delay_ratio": 0.0,
                        "hold_ratio": hold_ratio, "transport_delay_cycles": 0,
                        "hold_cycles": hold, "t_ref_cycles": tref, "control": True,
                    }
                    add_case(cases, seen, "P1C", "P1-C", group["fixture"], params, meta)


def expand_p1d(group: dict, cases: list[dict], seen: set[str]) -> None:
    for scenario in group["synthetic_scenarios"]:
        for limit in group["feeder_limits"]:
            params = base_params("G6")
            params.update({
                "scenario": scenario, "channels": 1, "capacity_mb": 256,
                "feeder_limit": limit, "handler_delay": 0,
                "observation_cycles": group["observation_cycles"], "n_tiles": 8,
                "watchdog_cycles": group["watchdog_cycles"],
            })
            meta = {
                "case_id": f"P1D_{scenario}_K{limit}",
                "scenario": scenario, "channels": 1, "feeder_limit": limit,
                "handler_delay": 0, "n_tiles": 8,
            }
            add_case(cases, seen, "P1D", "P1-D", group["fixture"], params, meta)
    if group["actual_ce"]:
        for limit in group["feeder_limits"]:
            for delay in group["handler_delays"]:
                params = base_params("G6")
                params.update({
                    "scenario": "ACTUAL_CE", "channels": 1, "capacity_mb": 256,
                    "feeder_limit": limit, "handler_delay": delay,
                    "observation_cycles": group["observation_cycles"], "n_tiles": 1,
                    "watchdog_cycles": group["watchdog_cycles"],
                })
                meta = {
                    "case_id": f"P1D_ACTUAL_CE_K{limit}_L{delay}",
                    "scenario": "ACTUAL_CE", "channels": 1, "feeder_limit": limit,
                    "handler_delay": delay, "n_tiles": 1,
                }
                add_case(cases, seen, "P1D", "P1-D", group["fixture"], params, meta)
    if group["two_channel"]:
        params = base_params("G6")
        params["system_ini"] = str(SYSTEM_2CH)
        params.update({
            "scenario": "TWO_CHANNEL", "channels": 2, "capacity_mb": 512,
            "feeder_limit": 8, "handler_delay": 0,
            "observation_cycles": group["observation_cycles"], "n_tiles": 8,
            "watchdog_cycles": group["watchdog_cycles"],
        })
        meta = {
            "case_id": "P1D_TWO_CHANNEL", "scenario": "TWO_CHANNEL", "channels": 2,
            "feeder_limit": 8, "handler_delay": 0, "n_tiles": 8,
        }
        add_case(cases, seen, "P1D", "P1-D", group["fixture"], params, meta)


def expand_p1e(group: dict, cases: list[dict], seen: set[str]) -> None:
    for n_tiles in group["n_tiles"]:
        for seed in range(group["seed_start"], group["seed_stop"] + 1):
            params = base_params("G7")
            params.update({
                "scenario": "POSITIVE", "trigger": group["trigger"],
                "negative_control": "NONE", "n_tiles": n_tiles, "seed": seed,
                "handler_delay": group["handler_delay_cycles"],
                "watchdog_cycles": group["watchdog_cycles"],
            })
            meta = {
                "case_id": f"P1E_REPORT_nt{n_tiles}_s{seed}", "kind": "positive",
                "n_tiles": n_tiles, "seed": seed, "trigger": group["trigger"],
                "negative_control": "NONE",
            }
            add_case(cases, seen, "P1E", "P1-E", group["fixture"], params, meta)
    for status in group["status_cases"]:
        params = base_params("G7")
        params["case"] = status
        meta = {"case_id": f"P1E_{status}", "kind": "status_negative", "status": status}
        status_fixture = "Phase1G7NegativeFixture.RunCase"
        add_case(cases, seen, "P1E", "P1-E", status_fixture, params, meta,
                 expected_negative=True, expected_status=status)
    for control in group["negative_controls"]:
        params = base_params("G7")
        params.update({
            "scenario": "POSITIVE", "trigger": "REPORT", "negative_control": control,
            "n_tiles": 1, "seed": 0, "handler_delay": group["handler_delay_cycles"],
            "watchdog_cycles": group["negative_control_watchdog"],
        })
        meta = {
            "case_id": f"P1E_{control}_nt1_s0", "kind": "negative_control",
            "negative_control": control, "n_tiles": 1, "seed": 0,
        }
        add_case(cases, seen, "P1E", "P1-E", group["fixture"], params, meta,
                 expected_negative=True)


def expand_p1f_cost(group: dict, calibration: dict, cases: list[dict], seen: set[str]) -> None:
    t_ref = calibration["p1f_t_ref"]
    for n_tiles in group["n_tiles"]:
        tref = int(t_ref[str(n_tiles)])
        for consumer in group["consumers"]:
            for seed in range(group["seed_start"], group["seed_stop"] + 1):
                for policy in group["policies"]:
                    hold = 0
                    if policy == "LOCAL_VISIBLE_H025T":
                        hold = math.ceil(0.25 * tref)
                    elif policy == "LOCAL_VISIBLE_H1T":
                        hold = tref
                    params = base_params("P1F_COST")
                    params.update({
                        "n_tiles": n_tiles, "seed": seed, "consumer": consumer,
                        "policy": policy, "hold_cycles": hold, "t_ref_cycles": tref,
                        "watchdog_cycles": group["watchdog_cycles"],
                    })
                    meta = {
                        "case_id": f"P1F_COST_nt{n_tiles}_{consumer.lower()}_s{seed}_{policy}",
                        "n_tiles": n_tiles, "seed": seed, "consumer": consumer,
                        "policy": policy, "hold_cycles": hold, "t_ref_cycles": tref,
                    }
                    add_case(cases, seen, "P1F_COST", "P1-F Cost", group["fixture"], params,
                             meta)


def expand_p1f_robust(group: dict, cases: list[dict], seen: set[str]) -> None:
    for capacity in group["queue_capacities"]:
        for interval in group["service_intervals"]:
            for drop_rate in group["drop_rates"]:
                params = base_params("P1F_ROBUST")
                params.update({
                    "queue_capacity": capacity, "service_interval_cycles": interval,
                    "drop_rate": drop_rate, "repeat_reads": group["repeat_reads"],
                    "transport_delay_cycles": group["transport_delay_cycles"],
                    "watchdog_cycles": group["watchdog_cycles"],
                })
                meta = {
                    "case_id": f"P1F_ROBUST_Q{capacity}_I{interval}_drop{drop_rate}",
                    "queue_capacity": capacity, "service_interval_cycles": interval,
                    "drop_rate": drop_rate, "repeat_reads": group["repeat_reads"],
                    "transport_delay_cycles": group["transport_delay_cycles"],
                }
                add_case(cases, seen, "P1F_ROBUST", "P1-F Robustness", group["fixture"],
                         params, meta)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", default=str(ROOT / "configs/suite_paper.yaml"))
    parser.add_argument("--out", default=str(MANIFEST))
    args = parser.parse_args()
    suite_path = Path(args.suite).resolve()
    suite = yaml.safe_load(suite_path.read_text())
    if suite.get("schema_version") != 1:
        raise ValueError("suite schema_version must be 1")
    if suite.get("suite") != "paper":
        raise ValueError("only the paper suite is supported by this expander")
    calibration_path = (ROOT / suite["calibration"]).resolve()
    calibration = json.loads(calibration_path.read_text())
    groups = suite.get("groups")
    if not isinstance(groups, dict):
        raise ValueError("suite groups must be a mapping")
    for name, group in groups.items():
        validate_group(name, group)

    cases: list[dict] = []
    seen: set[str] = set()
    expand_p1a(groups["P1A"], cases, seen)
    expand_p1b(groups["P1B"], cases, seen)
    expand_p1c(groups["P1C"], calibration, cases, seen)
    expand_p1d(groups["P1D"], cases, seen)
    expand_p1e(groups["P1E"], cases, seen)
    expand_p1f_cost(groups["P1F_COST"], calibration, cases, seen)
    expand_p1f_robust(groups["P1F_ROBUST"], cases, seen)

    CONFIG_ROOT.mkdir(parents=True, exist_ok=True)
    for case in cases:
        case_dir = CONFIG_ROOT / case["run_id"]
        case_dir.mkdir(parents=True, exist_ok=True)
        (case_dir / "config.json").write_text(json.dumps(case, indent=2) + "\n")

    by_group: dict[str, int] = {}
    expected_negative = 0
    for case in cases:
        by_group[case["group"]] = by_group.get(case["group"], 0) + 1
        if case["expected_negative"]:
            expected_negative += 1
    manifest = {
        "schema_version": 1,
        "suite": "paper",
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "source_yaml": str(suite_path.relative_to(ROOT)),
        "source_yaml_sha256": sha256_file(suite_path),
        "calibration_path": str(calibration_path.relative_to(ROOT)),
        "calibration_sha256": sha256_file(calibration_path),
        "upstream_commit": UPSTREAM_COMMIT,
        "expected_total": len(cases),
        "expected_negative": expected_negative,
        "expected_by_group": by_group,
        "cases": cases,
    }
    out_path = Path(args.out).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(manifest, indent=2) + "\n")

    with (ROOT / "configs/generated/paper-index.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["run_id", "group", "group_key", "fixture", "expected_negative",
                         "expected_status", "case_id"])
        for case in cases:
            writer.writerow([case["run_id"], case["group"], case["group_key"],
                             case["fixture"], case["expected_negative"],
                             case.get("expected_status", ""), case["case"].get("case_id", "")])
    print(f"expanded {len(cases)} cases ({expected_negative} expected negative)")
    for group, count in by_group.items():
        print(f"  {group}: {count}")
    print("G8_EXPAND=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
