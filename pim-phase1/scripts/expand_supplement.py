#!/usr/bin/env python3
"""Expand the pre-registered supplement matrix (P1-G and P1-H)."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import yaml

ROOT = Path(__file__).resolve().parent.parent
SIM_DIR = ROOT / "simulator"
DEVICE_INI = SIM_DIR / "ini/HBM2_samsung_2M_16B_x64.ini"
SYSTEM_1CH = ROOT / "configs/phase1_system_1ch.ini"
FULL_RUN_ROOT = ROOT / "runs/supplement"
SMOKE_RUN_ROOT = ROOT / "runs/supplement/smoke"
FULL_CONFIG_ROOT = ROOT / "configs/generated/supplement"
SMOKE_CONFIG_ROOT = ROOT / "configs/generated/supplement/smoke"
FULL_MANIFEST = ROOT / "configs/generated/supplement-manifest.json"
SMOKE_MANIFEST = ROOT / "configs/generated/supplement-smoke-manifest.json"
UPSTREAM_COMMIT = "3703d1f19c8f027360cc33a3243eb271e3bb6898"

GROUP_KEYS = {
    "P1G": {
        "fixture", "profile", "granularity", "n_tiles", "consumers", "seed_start",
        "seed_stop", "fault_configs", "delay_ratios", "hold_ratios",
        "handler_delay_cycles", "service_interval_cycles", "report_queue_capacity",
        "watchdog_cycles",
    },
    "P1H": {
        "fixture", "profile", "seed_start", "seed_stop", "scenarios", "granularities",
        "rounds", "transport_delay_cycles", "service_interval_cycles",
        "report_queue_capacity", "watchdog_cycles",
    },
}


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
             fixture: str, params: dict, case_meta: dict, run_root: Path,
             expected_negative: bool = False) -> None:
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
    out = run_root / run_id
    params = dict(params)
    params["output_dir"] = str(out)
    meta = dict(case_meta)
    meta["out"] = str(out)
    cases.append({
        "run_id": run_id,
        "group": group_label,
        "group_key": group_key,
        "fixture": fixture,
        "params": {k: str(v) for k, v in params.items()},
        "case": meta,
        "expected_negative": expected_negative,
    })


def expand_p1g(group: dict, calibration: dict, cases: list[dict], seen: set[str],
               run_root: Path, smoke: bool) -> None:
    t_ref = calibration["p1c_t_ref"]
    n_tiles_values = [1] if smoke else group["n_tiles"]
    consumers = ["HOST"] if smoke else group["consumers"]
    seeds = range(0, 3) if smoke else range(group["seed_start"], group["seed_stop"] + 1)
    delay_ratios = [0.0, 1.0] if smoke else group["delay_ratios"]
    hold_ratios = [0.0] if smoke else group["hold_ratios"]
    for n_tiles in n_tiles_values:
        tref = int(t_ref[str(n_tiles)])
        for consumer in consumers:
            for seed in seeds:
                target_burst, target_segment = target_burst_for(seed, n_tiles)
                for fault, placement in group["fault_configs"]:
                    for delay_ratio in delay_ratios:
                        for hold_ratio in hold_ratios:
                            delay = 0 if delay_ratio == 0 else math.ceil(delay_ratio * tref)
                            hold = 0 if hold_ratio == 0 else math.ceil(hold_ratio * tref)
                            params = base_params("P1G")
                            params.update({
                                "profile": group["profile"],
                                "granularity": group["granularity"],
                                "fault_kind": fault,
                                "escape_placement": placement,
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
                                "case_id": f"P1G_{fault}_{placement}_nt{n_tiles}_"
                                           f"{consumer.lower()}_s{seed}_"
                                           f"d{delay_ratio}_h{hold_ratio}",
                                "fault_kind": fault, "escape_placement": placement,
                                "n_tiles": n_tiles, "seed": seed, "consumer": consumer,
                                "target_burst": target_burst,
                                "target_segment": target_segment,
                                "delay_ratio": delay_ratio, "hold_ratio": hold_ratio,
                                "transport_delay_cycles": delay, "hold_cycles": hold,
                                "t_ref_cycles": tref,
                            }
                            add_case(cases, seen, "P1G", "P1-G Escape", group["fixture"],
                                     params, meta, run_root)


def expand_p1h(group: dict, cases: list[dict], seen: set[str], run_root: Path,
               smoke: bool) -> None:
    seeds = range(0, 1) if smoke else range(group["seed_start"], group["seed_stop"] + 1)
    for seed in seeds:
        rng = np.random.Generator(np.random.PCG64(seed))
        bank_p = int(rng.integers(0, 16))
        bank_r = int((bank_p + 1 + rng.integers(0, 15)) % 16)
        for granularity in group["granularities"]:
            for scenario in group["scenarios"]:
                scrub_values = [1] if scenario == "S6_SAME_BIT_RECUR" else [0, 1]
                for scrub in scrub_values:
                    params = base_params("P1H")
                    params.update({
                        "profile": group["profile"],
                        "scenario": scenario,
                        "granularity": granularity,
                        "scrub": scrub,
                        "seed": seed,
                        "bank_p": bank_p,
                        "bank_r": bank_r,
                        "rounds": group["rounds"],
                        "transport_delay_cycles": group["transport_delay_cycles"],
                        "service_interval_cycles": group["service_interval_cycles"],
                        "report_queue_capacity": group["report_queue_capacity"],
                        "watchdog_cycles": group["watchdog_cycles"],
                    })
                    meta = {
                        "case_id": f"P1H_{scenario}_{granularity}_scrub{scrub}_s{seed}",
                        "scenario": scenario, "granularity": granularity,
                        "scrub": scrub, "seed": seed, "bank_p": bank_p,
                        "bank_r": bank_r, "rounds": group["rounds"],
                    }
                    add_case(cases, seen, "P1H", "P1-H Sequence", group["fixture"],
                             params, meta, run_root)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", default=str(ROOT / "configs/suite_supplement.yaml"))
    parser.add_argument("--out", default="")
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()

    suite_path = Path(args.suite).resolve()
    suite = yaml.safe_load(suite_path.read_text())
    if suite.get("schema_version") != 1:
        raise ValueError("suite schema_version must be 1")
    if suite.get("suite") != "supplement":
        raise ValueError("only the supplement suite is supported")
    smoke = bool(args.smoke or suite.get("smoke", False))
    run_root = SMOKE_RUN_ROOT if smoke else FULL_RUN_ROOT
    config_root = SMOKE_CONFIG_ROOT if smoke else FULL_CONFIG_ROOT
    out_path = Path(args.out).resolve() if args.out else (
        SMOKE_MANIFEST if smoke else FULL_MANIFEST)
    calibration_path = (ROOT / suite["calibration"]).resolve()
    calibration = json.loads(calibration_path.read_text())
    groups = suite.get("groups")
    if not isinstance(groups, dict):
        raise ValueError("suite groups must be a mapping")
    for name, group in groups.items():
        validate_group(name, group)

    cases: list[dict] = []
    seen: set[str] = set()
    expand_p1g(groups["P1G"], calibration, cases, seen, run_root, smoke)
    expand_p1h(groups["P1H"], cases, seen, run_root, smoke)

    expected = {"P1-G Escape": 24, "P1-H Sequence": 33} if smoke else {
        "P1-G Escape": 4320, "P1-H Sequence": 330}
    by_group = Counter(case["group"] for case in cases)
    if dict(by_group) != expected:
        raise SystemExit(f"unexpected manifest counts: {dict(by_group)} expected {expected}")

    config_root.mkdir(parents=True, exist_ok=True)
    for case in cases:
        case_dir = config_root / case["run_id"]
        case_dir.mkdir(parents=True, exist_ok=True)
        (case_dir / "config.json").write_text(json.dumps(case, indent=2) + "\n")

    manifest = {
        "schema_version": 1,
        "suite": "supplement-smoke" if smoke else "supplement",
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "source_yaml": str(suite_path.relative_to(ROOT)),
        "source_yaml_sha256": sha256_file(suite_path),
        "calibration_path": str(calibration_path.relative_to(ROOT)),
        "calibration_sha256": sha256_file(calibration_path),
        "upstream_commit": UPSTREAM_COMMIT,
        "smoke": smoke,
        "expected_total": len(cases),
        "expected_negative": 0,
        "expected_by_group": expected,
        "cases": cases,
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(manifest, indent=2) + "\n")

    index_path = (config_root.parent / "supplement-index.csv" if not smoke
                  else config_root.parent / "supplement-smoke-index.csv")
    with index_path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["run_id", "group", "group_key", "fixture", "case_id",
                         "n_tiles", "seed", "consumer", "fault_kind",
                         "escape_placement", "scenario", "granularity", "scrub"])
        for case in cases:
            meta = case["case"]
            writer.writerow([
                case["run_id"], case["group"], case["group_key"], case["fixture"],
                meta.get("case_id", ""), meta.get("n_tiles", ""), meta.get("seed", ""),
                meta.get("consumer", ""), meta.get("fault_kind", ""),
                meta.get("escape_placement", ""), meta.get("scenario", ""),
                meta.get("granularity", ""), meta.get("scrub", ""),
            ])

    print(f"expanded {len(cases)} cases ({'smoke' if smoke else 'full'})")
    for group, count in expected.items():
        print(f"  {group}: {count}")
    print("G14_EXPAND=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
