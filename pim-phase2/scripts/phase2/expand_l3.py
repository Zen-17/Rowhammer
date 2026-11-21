#!/usr/bin/env python3
"""Expand the Phase 2 L3 acceptance cases and sweep into a run manifest."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = ROOT / "configs/phase2/l3_suite.yaml"
DEFAULT_OUT = ROOT / "configs/generated/phase2/l3-manifest.json"
RUN_ROOT = ROOT / "runs/phase2/l3"


def common_params(config: dict, workload: str, n_tiles: int, granularity: str,
                  fault_kind: str, fault_operand: int, fault_tile: int,
                  policy: str, transport_delay: int, **overrides) -> dict:
    accounting = config["common"]["accounting"]
    if isinstance(accounting, bool):
        accounting = "ON" if accounting else "OFF"
    observer = config["common"]["observer"]
    if isinstance(observer, bool):
        observer = "ON" if observer else "OFF"
    params = {
        "workload": workload,
        "n_tiles": n_tiles,
        "seed": overrides.pop("seed", 0),
        "granularity": granularity,
        "transport_delay": transport_delay,
        "service_interval": config["common"]["service_interval"],
        "capacity": config["common"]["capacity"],
        "handler_delay": config["common"]["handler_delay"],
        "fault_kind": fault_kind,
        "fault_operand": fault_operand,
        "fault_tile": fault_tile,
        "fault_bank": config["common"]["fault_bank"],
        "policy": policy,
        "radius_rows": config["common"]["radius_rows"],
        "negative_control": config["common"]["negative_control"],
        "watchdog_cycles": config["common"]["watchdog_cycles"],
        "device_ini": str(ROOT / "simulator/ini/HBM2_samsung_2M_16B_x64.ini"),
        "system_ini": str(ROOT / "configs/phase1_system_1ch.ini"),
        "accounting": accounting,
        "observer": observer,
        "kernel_mode": config["common"]["kernel_mode"],
        "max_log_entries": config["common"]["max_log_entries"],
        "checkpoint_missing": "",
        "release": config["common"]["release"],
        "hold_cycles": config["common"]["hold_cycles"],
    }
    params.update(overrides)
    return params


def make_case(case_id: str, group: str, params: dict, expect: dict,
              expected_negative: bool = False) -> dict:
    return {
        "run_id": case_id,
        "case_id": case_id,
        "group": group,
        "fixture": "Phase2E2EFixture.RunCase",
        "expected_negative": expected_negative,
        "params": params,
        "expect": expect,
    }


def build_manifest(config: dict) -> list[dict]:
    e2e = config["e2e"]
    cases: list[dict] = []
    delay = int(e2e["transport_delay"])
    common = config["common"]

    e2e0_variants = [
        ("tiles", "ON", "ON", "tiles", 8, "W1"),
        ("tiles", "OFF", "ON", "tiles", 8, "W1"),
        ("tiles", "ON", "OFF", "tiles", 8, "W1"),
        ("full", "ON", "ON", "full", 8, "W1"),
        ("segments", "ON", "ON", "segments", 8, "W1"),
        ("tiles", "ON", "ON", "tiles", 32, "W1"),
        ("tiles", "ON", "ON", "tiles", 8, "W2"),
        ("tiles", "ON", "ON", "tiles", 32, "W2"),
        ("full", "ON", "ON", "full", 8, "W2"),
        ("segments", "ON", "ON", "segments", 8, "W2"),
    ]
    for name, accounting, observer, kernel_mode, n_tiles, workload in e2e0_variants:
        case_id = f"e2e0_{workload.lower()}_n{n_tiles}_{name}_acc{accounting.lower()}_obs{observer.lower()}"
        params = common_params(config, workload, n_tiles, "page", "F0", 0, 0, "B1",
                               256 if n_tiles <= 8 else 512,
                               accounting=accounting, observer=observer,
                               kernel_mode=kernel_mode)
        cases.append(make_case(case_id, "E2E-0", params,
                               {"kind": "e2e0", "workload": workload,
                                "n_tiles": n_tiles, "accounting": accounting,
                                "observer": observer, "kernel_mode": kernel_mode}))

    for tile in e2e["fault_tile"]:
        params = common_params(config, "W1", 8, "page", "F2",
                               int(e2e["fault_operand_A"]), tile, "B1", delay)
        cases.append(make_case(f"e2e1_page_ue_k{tile}", "E2E-1", params,
                               {"kind": "e2e1", "fault_tile": tile, "n_tiles": 8}))
        params = dict(params)
        params["negative_control"] = "NO_REPLAY"
        cases.append(make_case(f"e2e1_no_replay_k{tile}", "E2E-1", params,
                               {"kind": "e2e1_no_replay", "fault_tile": tile},
                               expected_negative=True))
        params = common_params(config, "W1", 8, "page", "F2",
                               int(e2e["fault_operand_A"]), tile, "B0", delay)
        cases.append(make_case(f"e2e2_b0_k{tile}", "E2E-2", params,
                               {"kind": "e2e2", "fault_tile": tile, "n_tiles": 8}))

    tile = int(e2e["checkpoint_tile"])
    params = common_params(config, "W1", 8, "bank", "F2",
                           int(e2e["fault_operand_A"]), tile, "B1", delay)
    cases.append(make_case("e2e3_bank_ue_k3", "E2E-3", params,
                           {"kind": "e2e3", "fault_tile": tile, "n_tiles": 8}))
    params = common_params(config, "W1", 8, "page", "F1",
                           int(e2e["fault_operand_A"]), tile, "B2", delay)
    cases.append(make_case("e2e4_ce_b2_k3", "E2E-4", params,
                           {"kind": "e2e4", "fault_tile": tile, "n_tiles": 8}))

    w2_tile = int(e2e["w2_tile"])
    params = common_params(config, "W2", 8, "page", "F2",
                           int(e2e["fault_operand_T"]), w2_tile, "B1", delay)
    cases.append(make_case("e2e5_w2_t_ue_k2", "E2E-5", params,
                           {"kind": "e2e5", "fault_tile": w2_tile, "n_tiles": 8}))
    params = dict(params)
    params["negative_control"] = "NO_REPLAY"
    cases.append(make_case("e2e5_w2_t_no_replay_k2", "E2E-5", params,
                           {"kind": "e2e5_no_replay", "fault_tile": w2_tile},
                           expected_negative=True))
    params = common_params(config, "W1", 8, "page", "F2",
                           int(e2e["fault_operand_A"]), tile, "B1", delay,
                           negative_control="NO_RESTORE")
    cases.append(make_case("e2e6_no_restore_k3", "E2E-6", params,
                           {"kind": "e2e6", "fault_tile": tile},
                           expected_negative=True))
    params = common_params(config, "W1", 8, "page", "F2",
                           int(e2e["fault_operand_A"]), tile, "B1", delay,
                           checkpoint_missing=f"0:{tile}")
    cases.append(make_case("e2e7_checkpoint_missing_k3", "E2E-7", params,
                           {"kind": "e2e7", "fault_tile": tile},
                           expected_negative=True))

    sweep = config["sweep"]
    for workload in sweep["workloads"]:
        for n_tiles in sweep["n_tiles"]:
            for granularity in sweep["granularity"]:
                for fault_kind in sweep["fault_kind"]:
                    positions = {"first": 0, "middle": n_tiles // 2,
                                 "last": n_tiles - 1}
                    for position in sweep["fault_tile_positions"]:
                        tile_index = positions[position]
                        for policy in sweep["policies"]:
                            for seed in range(sweep["seeds"]["start"],
                                              sweep["seeds"]["stop"]):
                                case_id = (f"sweep_{workload.lower()}_n{n_tiles}_"
                                           f"{granularity}_{fault_kind}_{position}_"
                                           f"{policy}_s{seed}")
                                params = common_params(
                                    config, workload, n_tiles, granularity, fault_kind,
                                    int(sweep["fault_operand"]), tile_index, policy,
                                    int(sweep["transport_delay"]), seed=seed)
                                cases.append(make_case(case_id, "SWEEP", params,
                                                       {"kind": "sweep"}))
    return cases


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=str(DEFAULT_CONFIG))
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    args = parser.parse_args()
    config = yaml.safe_load(Path(args.config).read_text())
    cases = build_manifest(config)
    for case in cases:
        case["params"]["output_dir"] = str(RUN_ROOT / case["run_id"])
    counts: dict[str, int] = {}
    for case in cases:
        counts[case["group"]] = counts.get(case["group"], 0) + 1
    manifest = {
        "schema_version": 1,
        "suite": "phase2_l3",
        "config": str(Path(args.config).relative_to(ROOT)),
        "run_root": str(RUN_ROOT.relative_to(ROOT)),
        "expected_total": len(cases),
        "expected_by_group": counts,
        "cases": cases,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"L3_MANIFEST={out.relative_to(ROOT)}")
    print(f"L3_CASES={len(cases)}")
    for group, count in sorted(counts.items()):
        print(f"  {group}: {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
